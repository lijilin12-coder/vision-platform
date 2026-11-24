# 训练服务

平台自带的训练后端：用**合格的图像**训练一个小型卷积自编码器做异常检测，导出成 ONNX，交回模型仓库。
它是 `TrainingBackend` 端口的一个实现背后的那个服务（设计见 [docs/ddd/08-platforms-and-training.md](../../docs/ddd/08-platforms-and-training.md)，ADR-30）；
平台一侧调用它的代码在 `adapters/training_http/`。

| 文件 | 作用 |
|---|---|
| `trainer.py` | 训练和导出。可以单独运行，方便调试 |
| `server.py` | 一个很小的 HTTP 接口，把 `trainer.py` 包起来 |
| `test_service.py` | 这两个文件的测试 |
| `Dockerfile` | 打成镜像 |

## 1. 直接运行（推荐先这样）

```bash
cd services/training
python3 -m venv .venv
source .venv/bin/activate            # Windows：.venv\Scripts\activate
pip install torch --index-url https://download.pytorch.org/whl/cpu   # 只含 CPU 的版本，小得多；Mac 上直接 pip install torch
pip install numpy opencv-python-headless onnx
python server.py --port 8765 --work-dir ./work
```

看到“训练服务已启动”就可以了。另开一个终端，在仓库根目录：

```bash
export VISION_TRAINING_URL=http://127.0.0.1:8765      # Windows PowerShell：$env:VISION_TRAINING_URL = "http://127.0.0.1:8765"
CLI=./build/apps/vision-cli

$CLI sample generate /tmp/surface --ok 60 --ng 12     # 生成练习用的合成图像
$CLI sample import /tmp/surface/ok --label ok
$CLI sample import /tmp/surface/ng --label ng
$CLI dataset new 表面                                  # 输出数据集 ID
$CLI dataset add <数据集ID> --all-unassigned
$CLI dataset freeze <数据集ID>                         # 输出数据集版本 ID
$CLI train submit <数据集版本ID> --name surface-anomaly --epochs 120 --image-size 256 --wait
$CLI model list
```

训练完成后，模型自动进入模型仓库（状态是“未发布”），`model show` 能看到它是哪次训练、用哪个数据集版本训练出来的。

## 2. 用 Docker 运行

```bash
docker build -t vision-training services/training
docker run --rm -p 8765:8765 \
    -v /绝对路径/vision-data:/绝对路径/vision-data \
    vision-training --work-dir /绝对路径/vision-data/training-work
```

**`-v` 两边的路径必须相同**，`--work-dir` 也要在挂载的目录里。原因：平台和训练服务之间交接的是**文件路径**，不是文件内容
（图像可能很多、很大；这样不用通过 HTTP 传来传去）。所以两边必须能用同一个路径看到同一个文件。
平台的数据目录要用绝对路径：`vision-cli --data /绝对路径/vision-data ...`。

## 3. 模型是什么样的

- **输入** `image`：`[N, 1, S, S]`，float32，灰度，0～1。S 是训练时的 `imageSize`（默认 128，必须是 8 的倍数）。
  图像先转灰度（`cvtColor`），再缩放到 S×S（`resize`，`INTER_AREA`），再除以 255。
  **使用模型时必须做完全一样的预处理**：训练服务和平台里的推理算子用的都是 OpenCV 的同一种算法。
  （实测过：训练时用另一个库缩放，同一幅合格图像的得分差了 70%，直接越过了阈值。）
- **输出** `score`：`[N]`，这幅图像的异常得分，越大越不正常。
- **输出** `map`：`[N, 1, S, S]`，每个位置的异常程度。

“算差异”这一步包在模型里面了：使用模型的一方不需要知道它是自编码器，只管“图像进，得分出”。
训练报告里的 `threshold` 是建议的判定阈值（得分高于它算不合格），也写进了模型契约的 `numbers`。

阈值是怎么定的：只看训练集和验证集。两类能分开时，取“合格的最高分”和“不合格的最低分”的几何平均；
没有不合格的图像时，取合格最高分的 1.5 倍（报告的备注里会说明“没有验证过”）。**测试集不参与任何决定**，只用来最后报一个数。

## 4. 接口

| 请求 | 作用 |
|---|---|
| `POST /jobs` | 提交。请求体：`{"config": {"modelKind": "anomaly-autoencoder", "settings": {"epochs": 120, "imageSize": 256}}, "samples": [{"sampleId": "...", "ng": false, "split": "train", "path": "/绝对路径/图像"}]}`。返回 `{"ticket": "..."}` |
| `GET /jobs/<ticket>` | 查询。`{"state": "running"或"completed"或"failed", "progress": 0.4, "failure": "", "report": {...}, "output": {...}}` |
| `DELETE /jobs/<ticket>` | 取消 |
| `GET /health` | 服务是否活着 |

设置（`settings`）：`epochs`（默认 120）、`imageSize`（默认 128）、`batchSize`（默认 16）、`seed`（默认 1）。

样本划到哪一部分（`split`）是**平台决定的**，训练服务只照着用：这样划分方式记录在训练任务里，可以重现（不变量 T-3）。

## 5. 这个服务的局限（学习项目，够用就好）

- 一次只训练一个任务，其余的排队。
- 任务的状态只在内存里：**服务重启后，正在跑的训练就丢了**。平台查询时会得到“训练服务不认识这次训练”，把任务记为失败。
- 没有登录和权限：默认只监听本机（`--host 127.0.0.1`）。不要把它暴露到不可信的网络上。
- 模型很小、很简单：能发现“和合格图像明显不一样的地方”，对纹理复杂的产品不够用。更强的方法（例如 PatchCore）需要预训练权重，以后再说。

## 6. 运行测试

```bash
python -m unittest test_service -v
```
