# 算子一览

平台现在有 15 个算子。运行 `vision-cli operators` 可以看到每个算子的端口、参数和默认值；这里说的是**它们各自解决什么问题、怎么搭配**。

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
| 判定 | `vision.judge_count` | 连通块 → 判定依据 | 个数不超过上限为合格 | — |
| 判定 | `vision.judge_range` | 数值 → 判定依据 | 数值在范围之内为合格 | — |

端口类型必须对得上才能连线：图像只能接图像，区域只能接区域。每个方案至少要有一个判定算子，否则不能发布。

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

## 3. 几个容易用错的地方

| 现象 | 原因 |
|---|---|
| `normalize_brightness` 之后缺陷反而不明显了 | 它假设画面的大部分是稳定的背景。缺陷占了画面一半以上时，中位数落在缺陷上，不适用 |
| `adaptive_threshold` 只选中了一大片亮区的边缘 | 它比的是“周围”。亮区比 `blockSize` 还大时，亮区内部和周围一样亮。把 `blockSize` 调到明显大于目标 |
| `caliper_width` 量出来偏大 | 把划痕、污渍或画面里别的东西当成了边缘。提高 `edgeThreshold`，或者用 ROI 框小一点 |
| `caliper_width` 的 `found` 是 false | 对比度不够（降低 `edgeThreshold`），或者 ROI 里只有一条边 |
| `threshold` 报“需要灰度图像” | 前面忘了接 `to_gray` |

## 4. 现在还没有的

| 能力 | 为什么还没有 |
|---|---|
| 定位（模板匹配、形状匹配） | 模板是一幅图像，要随方案一起保存和发布。方案里现在只能存数字、布尔值和文字，**“方案附带的资源”还没有设计**（和模型的管理是同一类问题）。另外还需要一种新的端口类型来传“位置和角度” |
| 跟着定位结果移动 ROI | 依赖上一条 |
| 和标准图像比较（差分） | 同样需要“方案附带的资源” |
| 像素到毫米的换算 | 属于标定（BC-6），还没有开始 |
| 深度学习推理 | 属于模型仓库（BC-7）和推理运行时，另有 POC |
