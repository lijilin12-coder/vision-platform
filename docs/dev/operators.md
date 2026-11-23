# 算子一览

平台现在有 17 个算子。运行 `vision-cli operators` 可以看到每个算子的端口、参数和默认值；这里说的是**它们各自解决什么问题、怎么搭配**。

想自己写一个，见 [how-to-add-operator.md](how-to-add-operator.md)。原理在 [OpenCV 教程](../tutorial/opencv/README.md)里都讲过，下表最后一列是对应的课。

## 1. 按用途

| 分类 | 算子 | 输入 → 输出 | 做什么 | 教程 |
|---|---|---|---|---|
| 入口 | `vision.image_input` | → 图像 | 检测时相机的图像从这里进来 | 第 1 课 |
| 预处理 | `vision.to_gray` | 图像 → 图像 | 彩色转灰度。后面大多数算子要灰度图像 | 第 2 课 |
| 预处理 | `vision.invert` | 图像 → 图像 | 反色 | 第 2 课 |
| 预处理 | `vision.blur` | 图像 → 图像 | 平滑去噪：高斯、中值、均值 | 第 4 课 |
| 预处理 | `vision.normalize_brightness` | 图像 → 图像、倍数 | 把画面的中位灰度调到固定值，抵消光源整体变亮变暗 | 第 3 课 |
| 预处理 | `vision.edge_magnitude` | 图像 → 图像 | 边缘强度：灰度变化越剧烈越亮。找划痕、裂纹 | 第 6 课 |
| 分割 | `vision.threshold` | 图像 → 区域 | 固定阈值或大津法：比阈值亮（或暗）的像素 | 第 3 课 |
| 分割 | `vision.adaptive_threshold` | 图像 → 区域 | 比**周围**亮（或暗）的像素。光照不均匀时用 | 第 3 课 |
| 分割 | `vision.morphology` | 区域 → 区域 | 开、闭、腐蚀、膨胀：去噪点、填小洞 | 第 4 课 |
| 分析 | `vision.blob` | 区域 → 连通块、个数 | 把区域拆成一块一块，按面积筛选 | 第 5 课 |
| 分析 | `vision.blob_stats` | 连通块 → 个数、总面积、最大面积 | 给“范围判定”提供数字 | 第 5 课 |
| 分析 | `vision.region_area` | 区域 → 面积、百分比 | 区域有多大 | 第 5 课 |
| 测量 | `vision.caliper_width` | 图像 → 宽度、是否找到、两条边的位置 | 卡尺：找物体的两条外边缘，量它们的距离（亚像素） | 第 6 课 |
| 定位 | `vision.template_match` | 图像 → 位姿、得分、是否找到 | 模板匹配：在图像里找到事先框好的模板，知道零件在哪里、转了多少度 | 第 7～9 课 |
| 定位 | `vision.align_to_pose` | 图像 + 位姿 → 图像 | 按位姿把图像转正、移回标准位置 | 第 8 课 |
| 判定 | `vision.judge_count` | 连通块 → 判定依据 | 个数不超过上限为合格 | — |
| 判定 | `vision.judge_range` | 数值 → 判定依据 | 数值在范围之内为合格 | — |

端口类型必须对得上才能连线：图像只能接图像，区域只能接区域，位姿只能接位姿。每个方案至少要有一个判定算子，否则不能发布。

## 2. 常见的搭法

**找亮的（或暗的）缺陷**

```
image_input → to_gray → threshold → blob → judge_count
```

- 光源的亮度会变：在 `to_gray` 后面加 `normalize_brightness`
- 画面一边亮一边暗：`threshold` 换成 `adaptive_threshold`
- 有很多孤立的噪点：`threshold` 后面加 `morphology`（开运算），或者提高 `blob` 的最小面积
- 一个缺陷断成了几块：`threshold` 后面加 `morphology`（闭运算）

**按缺陷的总面积或最大面积判定**（而不是个数）

```
… → blob → blob_stats ─(totalArea 或 maxArea)→ judge_range
```

**找划痕**（不比周围亮多少，但边缘明显）

```
image_input → to_gray → blur → edge_magnitude → threshold → blob → judge_count
```

先 `blur` 再求边缘：不然噪声也会被当成边缘。

**量尺寸**

```
image_input → to_gray → caliper_width ─(width)→ judge_range
```

给 `caliper_width` 设置 ROI，只框住要量的那一段；不设置时量的是整幅画面里最外面的两条边。

**零件每次摆的位置不一样：先定位、对齐，再检测或测量**

```
image_input → to_gray ─┬→ template_match ─(score)→ judge_range「定位得分」
                       └──────(pose)──────→ align_to_pose → caliper_width → judge_range「宽度」
```

步骤：

1. 拿一张零件摆得正的图，转成灰度，从上面框一块有特征的地方作为匹配模板：`vision-cli recipe template add <方案ID> <名称> <样图> <x> <y> <宽> <高>`
2. 把模板交给 `template_match`：`vision-cli recipe set-param <方案ID> <节点> template <模板ID>`
3. `align_to_pose` 之后的图像里，零件回到了样图上的位置。后面的 ROI 照着样图画就行

把得分接到 `judge_range`（例如 0.7 ~ 1）上：零件没放好、放反了，这一条判不合格，操作员看得到原因。

**怎么框模板**（比调任何参数都重要）：

- 框**不对称**的特征：一个角、一个缺口、几个大小不同的孔。**不要只框一个圆、一条直边或者一片均匀的区域**——它们换个位置或角度看起来都一样，得分照样很高，位置和角度却是错的，而且算子自己发现不了
- 够用就好，不要太大：耗时主要由模板的大小决定
- 框的地方不要有每个零件都不一样的东西（序列号、划痕）

**让方案助手来调带定位的方案**

工作流 `locate-width-measure`（定位 → 对齐 → 卡尺 → 判定）可以直接交给方案助手。模板由你在任务书里给出：用基准图集里的哪个用例、框哪一块。

```bash
vision-cli workflow import samples/workflows/locate-width-measure.json
```

```bash
vision-cli golden sample ./locate-demo --scenario locate --blind-lighting same
```

```bash
vision-cli assist run ./locate-demo/brief.json --advisor search
```

示例图集里零件的位置随机偏 ±20 像素、转 ±8°。带定位的工作流 4 轮调到达标，盲测集也达标；同一批图交给不定位的 `width-measure`，怎么调都达不了标。

模板必须取自调参集的用例（任务书里用了盲测集的用例会被拒绝），而且要选一张零件摆得正的图。

## 3. 几个容易用错的地方

| 现象 | 原因 |
|---|---|
| `normalize_brightness` 之后缺陷反而不明显了 | 它假设画面的大部分是稳定的背景。缺陷占了画面一半以上时，中位数落在缺陷上，不适用 |
| `adaptive_threshold` 只选中了一大片亮区的边缘 | 它比的是“周围”。亮区比 `blockSize` 还大时，亮区内部和周围一样亮。把 `blockSize` 调到明显大于目标 |
| `caliper_width` 量出来偏大 | 把划痕、污渍或画面里别的东西当成了边缘。提高 `edgeThreshold`，或者用 ROI 框小一点 |
| `caliper_width` 的 `found` 是 false | 对比度不够（降低 `edgeThreshold`），或者 ROI 里只有一条边 |
| `threshold` 报“需要灰度图像” | 前面忘了接 `to_gray` |
| `template_match` 得分很高，但对齐之后位置明显不对 | 模板框的是对称的东西（圆、直边）。重新框一块有不对称特征的地方 |
| `template_match` 报“模板只适用于同样尺寸的图像” | 相机的分辨率和框模板时用的样图不一样。模板是按像素定义的，要在新的分辨率下重新框 |
| `template_match` 很慢 | 模板太大，或者 `angleRange` 设得比实际需要的大。5MP 图像上 150×110 的模板、±15° 约 150 毫秒；420×360 的模板要 0.7 秒 |
| 零件稍微转一点就找不到 | `angleRange` 是 0（不搜索角度），或者比零件实际转的角度小 |

## 4. 现在还没有的

| 能力 | 为什么还没有 |
|---|---|
| 形状匹配（基于边缘的定位） | 现在的模板匹配比的是灰度，光照变化大、有反光时不可靠。形状匹配更稳，但要另外实现 |
| 一次找多个相同的零件；零件有缩放 | 现在只返回得分最高的一个，不处理缩放 |
| 相对 ROI（图像不动，ROI 跟着位姿走） | 现在用“对齐图像”代替。实测对齐带来的测量误差在 0.15 像素以内，所以不急（[实测报告](../poc/locating-report.md) §3） |
| 和标准图像比较（差分） | 需要“标准图像”这个概念（和匹配模板类似，随方案保存），还没有设计 |
| 像素到毫米的换算 | 属于标定（BC-6），还没有开始 |
| 深度学习推理 | 属于模型仓库（BC-7）和推理运行时，另有 POC |
