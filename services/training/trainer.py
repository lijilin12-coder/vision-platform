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

import numpy as np
import torch
from PIL import Image
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
        error = (image - self.autoencoder(image)) ** 2
        anomaly_map = self.pool(error)
        score = torch.amax(anomaly_map, dim=(1, 2, 3))
        return score, anomaly_map


def load_image(path: str, size: int) -> np.ndarray:
    """读图像 → 灰度 → 缩放到 size×size → 0～1 的浮点数。使用模型时必须做同样的预处理（写进了模型契约）。"""
    with Image.open(path) as im:
        im = im.convert("L").resize((size, size), Image.BILINEAR)
        return np.asarray(im, dtype=np.float32) / 255.0


def auroc(ok_scores: list[float], ng_scores: list[float]) -> float | None:
    """ROC 曲线下的面积：随机取一个不合格的和一个合格的，不合格的得分更高的概率。1 最好，0.5 等于瞎猜。"""
    if not ok_scores or not ng_scores:
        return None
    wins = sum((n > o) + 0.5 * (n == o) for n in ng_scores for o in ok_scores)
    return wins / (len(ok_scores) * len(ng_scores))


def train(request: dict, out_dir: Path, progress=lambda fraction: None, cancelled=lambda: False) -> dict:
    """训练并导出。返回 {"report": ..., "output": ...}。出问题时抛异常，信息是给人看的。"""
    started = time.time()
    config = request.get("config", {})
    if config.get("modelKind") != MODEL_KIND:
        raise ValueError(f"这个训练服务只会训练 {MODEL_KIND}，不认识 {config.get('modelKind')!r}")
    settings = config.get("settings", {})
    epochs = int(settings.get("epochs", 40))
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

    # 建议的阈值：合格图像的得分上面留一些余量。
    # 定阈值只用训练集和验证集；测试集留着最后看一眼，不参与任何决定。
    reference_ok = train_scores + val_ok
    ok_max = max(reference_ok)
    threshold = ok_max * 1.5
    notes = []
    if seen_ng:
        ng_min = min(seen_ng)
        if ng_min > ok_max:
            threshold = (ok_max * ng_min) ** 0.5  # 两边能分开：取中间（几何平均，因为得分跨的数量级大）
        else:
            notes.append("训练集和验证集里，有不合格的图像得分不比合格的高：这个模型分不开它们")
    else:
        notes.append("训练集和验证集里没有不合格的图像：阈值只根据合格图像的得分定，没有验证过")

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
