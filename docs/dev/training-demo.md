# 训练闭环演示：从图像到产线上的模型

这份文档带你走一遍完整的闭环（POC-13）：

```
合成图像 ─► 样本 ─► 数据集版本 ─► 训练任务 ─► 模型版本 ─► 方案 ─► 上线包 ─► 工位检测
  （BC-9 数据）         （BC-12 训练）  （BC-7 模型仓库） （BC-2）   （BC-4）   （BC-5）
```

大约需要 10 分钟（不算第一次安装 PyTorch 的时间）。所有命令都在仓库根目录运行。

> 在 Linux 上完整验证过。Windows 上：第 3、4 步（用模型、上线、检测）由自动构建验证；**训练服务本身在 Windows 上还没有跑过**——遇到问题请告诉我。

## 0. 准备

**编译平台**（见 [build.md](build.md)）。第一次配置时 CMake 会自动下载 ONNX Runtime 的预编译库（约 120 MB）。

```bash
cmake --preset default     # Windows 用 cmake --preset windows
cmake --build build
```

**启动训练服务**（另开一个终端，让它一直开着）：

```bash
cd services/training
python3 -m venv .venv
source .venv/bin/activate                 # Windows：.venv\Scripts\activate
pip install torch --index-url https://download.pytorch.org/whl/cpu   # 只含 CPU 的版本，小得多。有 NVIDIA 显卡想用它时，去掉 --index-url 及后面的地址
pip install numpy opencv-python-headless onnx
python server.py --port 8765 --work-dir ./work
```

看到“训练服务已启动”就可以了。

**回到第一个终端**，告诉平台训练服务在哪里，并给命令起个短名字：

```bash
export VISION_TRAINING_URL=http://127.0.0.1:8765
CLI="./build/apps/vision-cli --data ./demo-data"
```

Windows PowerShell 里对应的写法：

```powershell
$env:VISION_TRAINING_URL = "http://127.0.0.1:8765"
function CLI { .\build\apps\vision-cli.exe --data .\demo-data @args }    # 之后把 $CLI 换成 CLI
```

## 1. 准备数据

```bash
$CLI sample generate ./demo-images --ok 60 --ng 12
```

生成 72 幅 640×480 的图像，可以打开看看：`demo-images/ok/` 里是合格的（深色零件，有的上面有几粒灰尘——灰尘不算缺陷），
`demo-images/ng/` 里是不合格的，有三种：亮斑（spot）、深色划痕（scratch）、污渍（stain）。

导入成样本，并标上合格 / 不合格：

```bash
$CLI sample import ./demo-images/ok --label ok
$CLI sample import ./demo-images/ng --label ng
```

建一个数据集，把样本放进去，冻结成一个**数据集版本**：

```bash
$CLI dataset new 表面                       # 输出数据集 ID，下面写作 <数据集ID>
$CLI dataset add <数据集ID> --all-unassigned
$CLI dataset freeze <数据集ID>              # 输出数据集版本 ID，形如 xxxxxxxx-...@v1
```

> **为什么要冻结？** 冻结之后这个版本里有哪些图像就定死了。以后任何时候都说得清“这个模型是用哪些图像训练的”。

## 2. 训练

```bash
$CLI train submit <数据集版本ID> --name surface-anomaly --image-size 256 --wait
```

大约半分钟。你会看到进度，最后是训练报告：

```
  ok_score_max = 0.0645      合格图像里最高的异常得分
  ng_score_min = 0.1008      不合格图像里最低的异常得分
  threshold    = 0.0806      建议的阈值：在上面两个数之间
  test_auroc   = 1           测试集上的 AUROC（1 最好，0.5 等于瞎猜）
  模型版本 surface-anomaly@v1
```

模型已经自动进了模型仓库：

```bash
$CLI model show surface-anomaly@v1
```

能看到它的**来源**：哪个训练任务、哪个数据集版本、怎么划分的训练集 / 验证集 / 测试集。状态是“未发布”。

> **模型只用合格的图像训练。** 它学的是“合格的零件长什么样”；给它一幅有缺陷的图像，缺陷的部分它还原不出来，差异大的地方就是异常。
> 所以它事先不需要知道缺陷长什么样——这是它和“阈值 + 连通域”那类方案最大的不同。原理见 [训练服务的说明](../../services/training/README.md)。

## 3. 在方案里用这个模型

仓库里有一个现成的方案描述（[samples/recipes/surface-anomaly.draft.json](../../samples/recipes/surface-anomaly.draft.json)）：

```
图像入口 ─► 灰度 ─► 异常检测（model = surface-anomaly@v1）─► 范围判定（异常倍数不超过 1）
```

```bash
$CLI recipe build samples/recipes/surface-anomaly.draft.json     # 输出方案 ID
```

先**调试运行**，看看模型在没见过的图像上表现如何。再生成一批新的图像（换一个随机种子，和训练用的不重复）：

```bash
$CLI sample generate ./demo-fresh --ok 20 --ng 9 --seed 7
$CLI recipe debug <方案ID> ./demo-fresh/ok/ok_001.png
$CLI recipe debug <方案ID> ./demo-fresh/ng/ng_003_stain.png
```

合格的那幅，异常倍数小于 1（例如 0.72）；有污渍的那幅大于 1（例如 1.46）。

> **未发布的模型只能在调试运行里用。** 这时候如果直接去检测，会被拒绝——刚训练出来的模型要先确认可用。

## 4. 发布、上线、检测

```bash
$CLI model publish surface-anomaly@v1       # 确认模型可用，发布
$CLI recipe publish <方案ID>                # 输出方案版本 ID
$CLI inspect <方案版本ID> ./demo-fresh/ok ./demo-fresh/ng
```

最后一行是汇总，应该是 `OK 20，NG 9，ERROR 0`。

再走一遍产线的流程——做成上线包，验收，部署到工位。只有验收通过的上线包才能部署；
这里直接拿那批全新的图像当基准图集（合格的一个目录、不合格的一个目录，正好是它要的样子）：

```bash
$CLI release create <方案版本ID>            # 输出上线包 ID
$CLI golden create 表面基准 --ok ./demo-fresh/ok --ng ./demo-fresh/ng   # 输出基准图集 ID
$CLI release accept <上线包ID> <基准图集ID>  # 应该是“通过”
$CLI station create 一号工位                # 输出工位 ID
$CLI station deploy <工位ID> <上线包ID>
$CLI station run <工位ID> ./demo-fresh/ok ./demo-fresh/ng
```

到这里，闭环走完了：**一批图像 → 一个训练出来的模型 → 在工位上判定合格与否**，而且每一步都留了记录，可以追溯。

## 5. 可以试试的

| 试一试 | 会看到 |
|---|---|
| 第 4 步里不执行 `model publish`，直接 `inspect` 或 `release create` | 被拒绝：“模型版本 … 还没有发布，只能在调试时使用” |
| 关掉训练服务再 `train submit` | 任务记录为“已失败”，原因写明；平台其他功能照常 |
| `train submit ... --epochs 20` | 训练不充分：合格和不合格的得分靠得很近，甚至分不开 |
| `train submit ... --image-size 128` | 图像缩得太小，细的划痕看不见了 |
| 用自己拍的照片 | 把合格品的照片放一个目录、不合格的放另一个目录，从第 1 步的 `sample import` 开始 |

## 6. 要知道的局限

- **合成图像很简单**，这里“全部判对”不代表在真实产品上也行。真实的表面有纹理、位置会偏、有反光，这个小模型多半不够用。
- 模型把图像缩到 256×256 再看，太小的缺陷（原图上小于 5 像素左右）看不见。
- 异常检测算子不支持 ROI：模型是用整幅图像训练的。
- 训练和推理的预处理必须完全一致。平台里这两处代码（`services/training/trainer.py` 的 `load_image`、`operators/src/anomaly_detect.cpp`）
  互相注明了对方，并且有一个测试专门核对两边算出的得分——改其中一处时测试会提醒你。
