"""训练一个小型卷积自编码器做异常检测，并导出成 ONNX。

原理（和推理教程里讲的是同一回事）：
  自编码器只用【合格】的图像训练，学会的是“把合格的图像压缩再还原”。
  给它一张有缺陷的图像，缺陷的部分它没见过，还原不出来——原图和还原图差得多的地方就是异常。

导出的 ONNX 模型把“算差异”也包在里面了：
  输入   image  [N, 1, S, S]  float32，灰度，取值 0～1
  输出   score  [N]           这幅图像的异常得分（越大越不正常）
         map    [N, 1, S, S]  每个位置的异常程度
这样使用模型的一方不需要知道它内部是自编码器，只需要“图像进，得分出”。

这个文件可以单独运行：
  python trainer.py request.json --out work/
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

import cv2
import numpy as np
import torch
from torch import nn

MODEL_KIND = "anomaly-autoencoder"


class Cancelled(Exception):
    """训练被取消。"""


class AutoEncoder(nn.Module):
    """三层下采样、三层上采样。故意做得很小：CPU 上几分钟能训完。"""

    def __init__(self, width: int = 16):
        super().__init__()
        w = width
        self.encoder = nn.Sequential(
            nn.Conv2d(1, w, 3, stride=2, padding=1), nn.ReLU(),
            nn.Conv2d(w, w * 2, 3, stride=2, padding=1), nn.ReLU(),
            nn.Conv2d(w * 2, w * 4, 3, stride=2, padding=1), nn.ReLU(),
        )
        self.decoder = nn.Sequential(
            nn.ConvTranspose2d(w * 4, w * 2, 4, stride=2, padding=1), nn.ReLU(),
            nn.ConvTranspose2d(w * 2, w, 4, stride=2, padding=1), nn.ReLU(),
            nn.ConvTranspose2d(w, 1, 4, stride=2, padding=1), nn.Sigmoid(),
        )

    def forward(self, x):
        return self.decoder(self.encoder(x))


class AnomalyScorer(nn.Module):
    """导出用的外壳：自编码器 + “算差异”。"""

    def __init__(self, autoencoder: AutoEncoder, smooth: int = 5):
        super().__init__()
        self.autoencoder = autoencoder
        # 把差异在一小块范围里取平均：孤立的一个噪点差异不大，一小片缺陷差异才大
        self.pool = nn.AvgPool2d(smooth, stride=1, padding=smooth // 2)

    def forward(self, image):
        # 差异取绝对值，不取平方：平方会把“一个很亮的像素”（比如一粒灰尘）放得很大，
        # 盖过“一小片不太亮的区域”（比如污渍）。绝对值再在小范围里取平均，看重的是“成片的差异”。
        error = torch.abs(image - self.autoencoder(image))
        anomaly_map = self.pool(error)
        score = torch.amax(anomaly_map, dim=(1, 2, 3))
        return score, anomaly_map


def load_image(path: str, size: int) -> np.ndarray:
    """读图像 → 灰度 → 缩放到 size×size → 0～1 的浮点数。

    使用模型时必须做【完全一样】的预处理，否则得分对不上（实测：缩放的算法不同，合格图像的得分就能差出 70%）。
    所以这里用 OpenCV，和平台里的推理算子是同一个库、同一种算法：
      彩色转灰度  cvtColor(BGR2GRAY)
      缩放        resize(INTER_AREA)   —— 缩小时对覆盖到的像素取平均
      归一化      除以 255
    平台里对应的代码在推理算子里；改这里就必须同时改那里。
    """
    # 不用 cv2.imread：它在 Windows 上打不开带中文的路径
    image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_UNCHANGED)
    if image is None:
        raise ValueError("不是能识别的图像文件")
    if image.dtype != np.uint8:
        raise ValueError("只支持每通道 8 位的图像")
    if image.ndim == 3:
        image = cv2.cvtColor(image, cv2.COLOR_BGRA2GRAY if image.shape[2] == 4 else cv2.COLOR_BGR2GRAY)
    image = cv2.resize(image, (size, size), interpolation=cv2.INTER_AREA)
    return image.astype(np.float32) / 255.0


def auroc(ok_scores: list[float], ng_scores: list[float]) -> float | None:
    """ROC 曲线下的面积：随机取一个不合格的和一个合格的，不合格的得分更高的概率。1 最好，0.5 等于瞎猜。"""
    if not ok_scores or not ng_scores:
        return None
    wins = sum((n > o) + 0.5 * (n == o) for n in ng_scores for o in ok_scores)
    return wins / (len(ok_scores) * len(ng_scores))


def best_threshold(ok_scores: list[float], ng_scores: list[float]) -> tuple[float, tuple[float, float]]:
    """两类得分有重叠时的阈值：让“漏检率 + 误报率”最小。返回 (阈值, (漏检率, 误报率))。

    两种错误各自按比例算，不按个数：不合格的图像通常比合格的少得多，按个数算的话，
    “全部判合格”就是错得最少的，那样的阈值没有用。
    候选的位置是相邻两个得分的中点。并列时取较低的阈值——宁可多报，不要漏。
    """
    values = sorted(set(ok_scores) | set(ng_scores))
    candidates = [(a + b) / 2 for a, b in zip(values, values[1:])] or [values[0]]
    best = None
    for t in candidates:
        miss = sum(s <= t for s in ng_scores) / len(ng_scores)
        false_alarm = sum(s > t for s in ok_scores) / len(ok_scores)
        if best is None or miss + false_alarm < best[1][0] + best[1][1] - 1e-12:
            best = (t, (miss, false_alarm))
    return best


def train(request: dict, out_dir: Path, progress=lambda fraction: None, cancelled=lambda: False) -> dict:
    """训练并导出。返回 {"report": ..., "output": ...}。出问题时抛异常，信息是给人看的。"""
    started = time.time()
    config = request.get("config", {})
    if config.get("modelKind") != MODEL_KIND:
        raise ValueError(f"这个训练服务只会训练 {MODEL_KIND}，不认识 {config.get('modelKind')!r}")
    settings = config.get("settings", {})
    epochs = int(settings.get("epochs", 120))
    size = int(settings.get("imageSize", 128))
    batch_size = int(settings.get("batchSize", 16))
    if size % 8 != 0 or size < 32:
        raise ValueError("imageSize 必须是 8 的倍数，并且不小于 32")
    if epochs < 1:
        raise ValueError("epochs 至少是 1")
    seed = int(settings.get("seed", 1))
    torch.manual_seed(seed)
    np.random.seed(seed)

    # 样本已经由平台分好了（训练集 / 验证集 / 测试集）。这里只管读。
    groups: dict[tuple[str, bool], list[np.ndarray]] = {}
    for sample in request.get("samples", []):
        try:
            image = load_image(sample["path"], size)
        except Exception as e:  # noqa: BLE001
            raise ValueError(f"读不了样本 {sample.get('sampleId', '?')[:12]} 的图像：{e}") from e
        groups.setdefault((sample["split"], bool(sample["ng"])), []).append(image)

    train_ok = groups.get(("train", False), [])
    if len(train_ok) < 4:
        raise ValueError(f"训练集里合格的图像只有 {len(train_ok)} 幅，太少了")

    model = AutoEncoder()
    optimizer = torch.optim.Adam(model.parameters(), lr=2e-3)
    data = torch.from_numpy(np.stack(train_ok)[:, None, :, :])
    loss_value = 0.0
    for epoch in range(epochs):
        if cancelled():
            raise Cancelled()
        order = torch.randperm(len(data))
        total = 0.0
        for start in range(0, len(data), batch_size):
            batch = data[order[start:start + batch_size]]
            # 轻微的亮度抖动：让模型别把“整体亮一点、暗一点”当成异常
            batch = (batch * (0.9 + 0.2 * torch.rand(len(batch), 1, 1, 1))).clamp(0, 1)
            loss = nn.functional.mse_loss(model(batch), batch)
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()
            total += loss.item() * len(batch)
        loss_value = total / len(data)
        progress((epoch + 1) / epochs * 0.9)

    scorer = AnomalyScorer(model).eval()

    def scores(images: list[np.ndarray]) -> list[float]:
        if not images:
            return []
        with torch.no_grad():
            s, _ = scorer(torch.from_numpy(np.stack(images)[:, None, :, :]))
        return [float(v) for v in s]

    train_scores = scores(train_ok)
    val_ok, val_ng = scores(groups.get(("val", False), [])), scores(groups.get(("val", True), []))
    test_ok, test_ng = scores(groups.get(("test", False), [])), scores(groups.get(("test", True), []))
    # 训练集里不合格的图像没有用来训练（自编码器只学合格的），可以拿来帮忙定阈值
    seen_ng = scores(groups.get(("train", True), [])) + val_ng

    # 建议的阈值。定阈值只用训练集和验证集；测试集留着最后看一眼，不参与任何决定。
    reference_ok = train_scores + val_ok
    ok_max = max(reference_ok)
    threshold = ok_max * 1.5
    notes = []
    expected = None  # 两类分不开时：在这个阈值上，预计的漏检率和误报率
    if not seen_ng:
        notes.append("训练集和验证集里没有不合格的图像：阈值只根据合格图像的得分定，没有验证过")
    elif min(seen_ng) > ok_max:
        # 两边能分开：取中间（几何平均，因为得分跨的数量级大）
        threshold = (ok_max * min(seen_ng)) ** 0.5
    else:
        # 两边分不开：没有哪个阈值能全对。找一个“漏检率和误报率加起来最小”的位置。
        # （2026-11-24 以前这里简单地取合格最高分的 1.5 倍——在真实数据上，这等于一个不合格的都检不出来。）
        # 合格的一侧优先用验证集：训练集里的合格图像模型见过，得分偏低，拿它们定阈值会偏紧、到了新图像上误报多。
        ok_side = val_ok if len(val_ok) >= 5 else reference_ok
        threshold, expected = best_threshold(ok_side, seen_ng)
        notes.append(
            "合格和不合格的图像得分有重叠，这个模型分不开它们。阈值取在两种错误加起来最少的位置："
            f"在定阈值用的图像上，漏检 {expected[0] * 100:.0f}%、误报 {expected[1] * 100:.0f}%"
        )
        if expected[0] + expected[1] > 0.5:
            # 两种错误率加起来超过一半：和随便猜差不了多少。直接说，别让人以为这个模型能用
            notes.append("效果很差，不建议使用这个模型：这种小模型看不出这类产品上的缺陷")

    metrics = {
        "train_loss": loss_value,
        "epochs": float(epochs),
        "image_size": float(size),
        "train_ok": float(len(train_ok)),
        "ok_score_max": ok_max,
        "threshold": threshold,
    }
    if seen_ng:
        metrics["ng_score_min"] = min(seen_ng)
    if expected is not None:
        metrics["threshold_miss_rate"] = expected[0]
        metrics["threshold_false_alarm_rate"] = expected[1]
    for name, ok, ng in (("val", val_ok, val_ng), ("test", test_ok, test_ng)):
        value = auroc(ok, ng)
        if value is not None:
            metrics[f"{name}_auroc"] = value
        elif ok or ng:
            notes.append(f"{'验证集' if name == 'val' else '测试集'}里只有一类图像，算不出 AUROC")
    if test_ok or test_ng:
        wrong = sum(s > threshold for s in test_ok) + sum(s <= threshold for s in test_ng)
        metrics["test_errors"] = float(wrong)
        metrics["test_count"] = float(len(test_ok) + len(test_ng))

    progress(0.95)
    out_dir.mkdir(parents=True, exist_ok=True)
    model_file = out_dir / "model.onnx"
    example = torch.zeros(1, 1, size, size)
    export_options = dict(
        input_names=["image"], output_names=["score", "map"],
        dynamic_axes={"image": {0: "batch"}, "score": {0: "batch"}, "map": {0: "batch"}},
        opset_version=17,
    )
    try:
        # 新版 PyTorch 默认换了一种导出方式，需要额外的依赖；这个模型很简单，用原来的方式就够了
        torch.onnx.export(scorer, (example,), str(model_file), dynamo=False, **export_options)
    except TypeError:  # 旧版 PyTorch 没有 dynamo 这个参数
        torch.onnx.export(scorer, (example,), str(model_file), **export_options)
    progress(1.0)
    return {
        "report": {"metrics": metrics, "seconds": time.time() - started, "notes": "；".join(notes)},
        "output": {
            "modelFile": str(model_file.resolve()),
            "contractKind": MODEL_KIND,
            "inputWidth": size, "inputHeight": size, "inputChannels": 1,
            "outputMeaning": "anomaly-score-and-map",
            "numbers": {"threshold": threshold, "ok_score_max": ok_max},
        },
    }


def main() -> int:
    if len(sys.argv) < 2:
        print("用法：python trainer.py <请求.json> [--out <输出目录>]", file=sys.stderr)
        return 2
    out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else Path("work")
    request = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    result = train(request, out, progress=lambda f: print(f"  {f * 100:.0f}%", flush=True))
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
