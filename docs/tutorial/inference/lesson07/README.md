# 第 7 课：推理后端与 NPU

> 预计用时：2.5 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

前六课自己写了一个推理引擎的各个部件。项目里不会用自己写的引擎，而是用现成的：开发机和 Windows、Linux 上用 **ONNX Runtime**，鸿蒙电脑上用 **MindSpore Lite**（[技术架构](../../../02-architecture.md)第 5 节）。这一课讲怎么把它们接进项目，以及它们怎么把模型放到 NPU 上运行。

1. 两个推理引擎的接口完全不同，项目的代码怎么组织，才能不写两遍？
2. 模型交给推理引擎以后，引擎怎么决定哪些算子跑在 NPU 上、哪些留在 CPU 上？
3. 同一个模型在两个引擎上算出来的结果不一样，怎么办？
4. 为什么第一次推理特别慢？

招聘要求里的“NPU runtime 开发”“针对特定硬件（国产加速芯片）进行算子适配”“端侧部署经验”，对应的就是这一课。

## 1. 先对一下名字

真实引擎里的概念，前六课都见过，只是名字不同：

| 前六课里的东西 | ONNX Runtime 里 | 这一课要补的 |
|---|---|---|
| 张量（第 1 课） | `Ort::Value` | |
| 计算图、执行计划（第 2 课） | 模型文件 `.onnx`；加载后是一个 `Ort::Session` | |
| 图优化的遍（第 3 课） | 图优化级别（graph optimization level） | 第 3 节实际看一看 |
| 内存规划（第 4 课） | 内存池（arena）和内存复用计划 | |
| 算子在某种硬件上的实现（第 5 课） | kernel | |
| 量化（第 6 课） | 量化工具；QDQ 格式的模型 | |
| — | **execution provider**：一种硬件对应一个 | 第 4 节 |

MindSpore Lite 里对应的是：模型文件 `.ms`、`Model`、`Context` 和其中的设备信息（device info）。

## 2. 项目的接口：`InferBackend`

### 不这样做会怎样

最直接的写法，是在“异常检测”算子里直接调用 ONNX Runtime 的函数。然后到了鸿蒙上，再用 `#ifdef` 加一份 MindSpore Lite 的写法。每多一个用到模型的算子，就多一处 `#ifdef`；每多一个平台，所有这些地方都要改；单元测试必须带着真的引擎和真的模型文件才能跑。

### 怎么做

这正是 C++ 教程第 8 课讲的“依赖倒置”：**核心代码定义一个接口，说清楚“我需要什么”；每个引擎写一个适配器来实现它。** 架构文档里这个接口叫 `InferBackend`：

| 端口（领域层定义） | 鸿蒙电脑 | Linux | Windows | macOS（仅开发） |
|---|---|---|---|---|
| `InferBackend` | MindSpore Lite | ONNX Runtime | ONNX Runtime | ONNX Runtime |

它在项目里还没有实现（ADR-11 的状态是“有效，待 POC”），下面是示例 [`backend.cpp`](backend.cpp) 里的草图，可以当作讨论的起点：

```cpp
class InferBackend {
public:
    virtual ~InferBackend() = default;
    virtual std::string name() const = 0;
    // 加载模型。失败时返回 false，原因写进 error。真实的接口里参数是模型文件的路径。
    virtual bool load(const Model& model, std::string* error) = 0;
    // 推理一次。必须先 load 成功。
    virtual Tensor infer(const Tensor& input) = 0;
    virtual InferStats stats() const = 0;
};
```

真正定下来之前，有几件事要想清楚。它们比函数签名重要：

| 问题 | 考虑 |
|---|---|
| 接口里出现什么类型 | 只能出现项目自己的类型（图像、张量、`Result`）。`Ort::Value` 之类的类型一旦出现在接口里，核心代码就依赖 ONNX Runtime 了 |
| 模型文件 | 两个引擎的格式不同（`.onnx` 和 `.ms`）。同一个模型的两种格式怎么对应、怎么放进上线包，是模型工具链和发布要管的事 |
| 错误怎么报 | 加载失败是常见情况（文件不存在、格式不对、有不支持的算子），用 `Result` 而不是异常（C++ 教程第 6 课） |
| 线程 | 很多引擎在用到 GPU、NPU 时，要求加载、推理、释放在同一个线程。C++ 教程第 16 课的推理服务就是按这个约束写的：一个推理线程拥有模型，其他线程提交任务 |
| 输入输出的形状 | 调用方要能问“这个模型要多大的输入”，否则预处理没法写 |
| 预热 | 放在 `load` 里做，还是单独一个函数？见第 7 节 |
| 能不能知道模型实际跑在哪里 | 要。示例里的 `stats()` 报告有几个算子回退到了 CPU。否则“NPU 没用上”这件事只能从“怎么这么慢”猜出来 |

## 3. ONNX Runtime 的基本用法

下面的片段**不在自动构建里**：需要先安装 ONNX Runtime（[技术调研](../../../03-tech-research.md)第 6 节：Mac 上用 Homebrew 安装，其他系统用官方预编译包）。函数名对照过官方头文件 `onnxruntime_cxx_api.h`，细节以[官方文档](https://onnxruntime.ai/docs/api/c/)为准。

```cpp
#include <onnxruntime_cxx_api.h>

// 1. 环境和选项。整个程序一个 Env 就够
Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "vision");
Ort::SessionOptions options;
options.SetIntraOpNumThreads(4);                                            // 一个算子内部用几个线程
options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);  // 图优化级别

// 2. 加载模型：读文件、做图优化、给每个算子选好实现、规划内存。这一步慢，只做一次
Ort::Session session(env, ORT_TSTR("model.onnx"), options);

// 3. 输入输出的名字
Ort::AllocatorWithDefaultOptions allocator;
Ort::AllocatedStringPtr inputName = session.GetInputNameAllocated(0, allocator);
Ort::AllocatedStringPtr outputName = session.GetOutputNameAllocated(0, allocator);

// 4. 把自己的内存包装成张量。不复制：input 必须活到 Run 结束
std::vector<float> input(1 * 3 * 224 * 224);
const std::array<int64_t, 4> shape{1, 3, 224, 224};
Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());

// 5. 推理
const char* inputNames[] = {inputName.get()};
const char* outputNames[] = {outputName.get()};
std::vector<Ort::Value> outputs = session.Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 1);
const float* scores = outputs[0].GetTensorData<float>();
```

两个容易踩的地方：

- **模型路径在 Windows 上是宽字符**（所以有 `ORT_TSTR` 这个宏）。项目内部的路径统一是 UTF-8 的 `std::string`（[`path.h`](../../../../core/common/include/vision/common/path.h)），适配器里要通过 `std::filesystem::path` 转换，否则带中文的路径会加载失败。
- **`CreateTensor` 不复制数据。** 它只是记下了指针。这对性能是好事（图像不用多拷贝一次），但那块内存的生命周期要自己保证（C++ 教程第 4 课）。

### 看一看图优化做了什么

[第 3 课](../lesson03/README.md)自己写了“把 BatchNorm 合并进卷积”和“把 ReLU 合并进卷积”。ONNX Runtime 可以把优化之后的模型存成文件，于是能直接看到它做了同样的事。[`ort_demo.py`](ort_demo.py) 构造一个很小的模型，分别用三个优化级别加载（需要 `pip install onnx onnxruntime numpy`；下面是 onnxruntime 1.29 的输出）：

```
这台机器上可用的 execution provider： ['AzureExecutionProvider', 'CPUExecutionProvider']

不优化： Conv → BatchNormalization → Relu → GlobalAveragePool → Flatten → Gemm
      和不优化时的输出相比，最大误差 0.0e+00
基本： Conv → Relu → GlobalAveragePool → Flatten → Gemm
      和不优化时的输出相比，最大误差 9.5e-07
扩展： FusedConv → GlobalAveragePool → Flatten → Gemm
      和不优化时的输出相比，最大误差 9.5e-07

连续推理 5 次的耗时（毫秒）： 1.91 0.50 0.39 0.39 0.47
```

- “基本”级别：`BatchNormalization` 不见了，被并进了 `Conv` 的权重。
- “扩展”级别：`Conv` 和 `Relu` 变成了一个 `FusedConv`。这个算子不在 ONNX 的标准里，是 ONNX Runtime 自己的。
- 优化前后的输出差了 9.5e-07：不是逐位相同，但足够接近。第 6 节会回到这个话题。

再高一级（`ORT_ENABLE_ALL`）时，卷积会被换成另一种内存布局的实现，ONNX Runtime 同时会警告：这样保存下来的模型**可能含有针对当前硬件的优化，只应该在同样的环境里使用**。这提醒我们：优化后的模型不是可以随便搬到别的机器上的东西。

## 4. execution provider，以及子图划分和 CPU 回退

### 一种硬件一个 provider

ONNX Runtime 把“在某种硬件上执行算子”的能力抽象成 **execution provider**：CPU 一个，英伟达的 GPU 一个（CUDA），各家的 NPU 各一个。创建会话时按优先顺序给出一个列表，比如“先 NPU，再 CPU”。

问题在于，**加速硬件支持的算子总是比 CPU 少**。鸿蒙的官方文档里有同样的话：NNRT 对接的加速硬件“推理能力较强，但支持的算子规格少”，而 CPU“推理能力较弱，但支持算子规格更全”。模型里只要有一个 NPU 不支持的算子，引擎就得做一个决定。

### 划分

引擎的做法是**子图划分**：

1. 按优先顺序问每个 provider：“这张图里，哪些算子你能做？”
2. 把一个 provider 认领的、连在一起的算子划成一个**子图**，整个交给它。
3. 没有被认领的算子，交给 CPU。CPU 支持全部算子，是兜底的。这叫 **CPU 回退**（fallback）。

示例 [`backend.cpp`](backend.cpp) 用一条算子链模拟了这个过程（真实的模型是图，划分更复杂，道理相同）：

```cpp
std::vector<Segment> partition(const Model& model, const Device* accelerator, const Device& cpu) {
    std::vector<Segment> segments;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const Device* device = (accelerator != nullptr && accelerator->supports(model.nodes[i])) ? accelerator : &cpu;
        if (!segments.empty() && segments.back().device == device) {
            segments.back().end = i + 1;                    // 和上一段是同一个设备：合并
        } else {
            segments.push_back(Segment{device, i, i + 1});  // 换设备了：新开一段
        }
    }
    return segments;
}
```

### 一个不支持的算子能造成多大的影响

示例里有两个模型，区别只是激活函数：A 用 `Relu`，B 用 `HardSwish`。假想的 NPU 不支持 `HardSwish`。

**下面的耗时是示例里假设的数字**（NPU 上每个算子 1 ms，CPU 上 5 ms，每切换一次设备 12 ms），不是测量结果，只用来说明问题的结构：

```
模型 A（全部受支持）（8 个算子）
  纯 CPU 后端：切成 1 段，切换 0 次，0 个算子回退到 CPU，估算 40 ms
      [CPU] Conv BatchNorm Relu Conv BatchNorm Relu Pool Gemm
  NPU + CPU 回退：切成 1 段，切换 0 次，0 个算子回退到 CPU，估算 8 ms
      [NPU] Conv BatchNorm Relu Conv BatchNorm Relu Pool Gemm
  一致性：两个后端输出的最大误差 5.73e-04，通过（阈值 1e-3）

模型 B（激活函数是 HardSwish）（8 个算子）
  纯 CPU 后端：切成 1 段，切换 0 次，0 个算子回退到 CPU，估算 40 ms
      [CPU] Conv BatchNorm HardSwish Conv BatchNorm HardSwish Pool Gemm
  NPU + CPU 回退：切成 5 段，切换 4 次，2 个算子回退到 CPU，估算 64 ms
      [NPU] Conv BatchNorm
      [CPU] HardSwish
      [NPU] Conv BatchNorm
      [CPU] HardSwish
      [NPU] Pool Gemm
  一致性：两个后端输出的最大误差 2.89e-04，通过（阈值 1e-3）
```

模型 B 里只有 2 个算子不支持，8 个里有 6 个确实跑在 NPU 上。但模型被切成了 5 段，设备切换了 4 次。每次切换，数据都要在 CPU 的内存和 NPU 的内存之间搬一趟，可能还要转换格式。在示例假设的数字下，**“用了 NPU”的结果（64 ms）比纯 CPU（40 ms）还慢**。

真实的数字要在设备上测，但这个结构是真实的：**回退的代价不在于那几个算子慢，而在于它们把图切碎了。**

### 能做什么

| 做法 | 说明 |
|---|---|
| 选模型时就避开 | [技术调研](../../../03-tech-research.md)的结论：优先选“结构简单、算子常见”的网络（ResNet、MobileNet 一类） |
| 导出时替换算子 | 把不支持的算子换成等价的、受支持的写法；或者换一个激活函数重新训练 |
| 让问题暴露出来 | 加载时报告划分结果。示例的 `stats()` 给出段数、切换次数、回退的算子数 |
| 不允许悄悄回退 | 示例里的“不允许回退”模式：有不支持的算子就加载失败，并说出是哪些。产线上“明确地失败”比“悄悄地变慢”好 |
| 给硬件补上这个算子 | 这就是芯片公司里“算子适配”的工作：在 NPU 上实现缺的那个算子（第 5 课的内容），让它不再回退 |

```
不允许回退时加载模型 B：失败
  有 2 个算子 NPU 不支持：HardSwish HardSwish
```

## 5. 鸿蒙上：MindSpore Lite 加 NNRt

鸿蒙上的推理引擎是系统自带的 MindSpore Lite，它通过 **NNRt**（Neural Network Runtime，官方文档里写作 NNRT）对接 NPU。NNRt 是系统提供的一层统一接口，下面是各家芯片的驱动。

下面的流程和函数名来自鸿蒙官方指南[《使用 MindSpore Lite 进行模型推理 (C/C++)》](https://github.com/openharmony/docs/blob/master/zh-cn/application-dev/ai/mindspore/mindspore-lite-guidelines.md)。项目里还没有在真机上跑过（POC-4），**写代码时以官方指南和真机上的实际表现为准**。

| 步骤 | 做什么 | 官方指南里用到的函数 |
|---|---|---|
| 1 | 准备 `.ms` 格式的模型 | 用 `converter_lite` 从 ONNX 转换（模型工具链，POC-10） |
| 2 | 创建上下文，设置线程数 | `OH_AI_ContextCreate`、`OH_AI_ContextSetThreadNum` |
| 3 | 往上下文里添加设备：**先加 NNRt，再加 CPU** | `OH_AI_CreateNNRTDeviceInfoByType`、`OH_AI_DeviceInfoCreate`、`OH_AI_ContextAddDeviceInfo` |
| 4 | 加载并编译模型 | `OH_AI_ModelCreate`、`OH_AI_ModelBuildFromFile` |
| 5 | 取得输入张量，填入数据 | `OH_AI_ModelGetInputs`、`OH_AI_TensorGetMutableData` |
| 6 | 推理 | `OH_AI_ModelPredict` |
| 7 | 读取输出 | `OH_AI_TensorGetData` |
| 8 | 释放 | `OH_AI_ModelDestroy`、`OH_AI_ContextDestroy` |

和 ONNX Runtime 对比着看，结构是一样的：环境和选项 → 加载（编译）→ 填输入 → 推理 → 读输出。**两边能对上，才写得出一个共同的 `InferBackend` 接口。**

第 3 步就是上一节的内容：官方指南把“同时添加 NNRt 和 CPU”叫作**异构推理**，添加的顺序就是优先顺序，NNRt 做不了的算子由 CPU 接手。

技术调研里标了“需要注意”的几项，都和这一课有关：

- 模型转换有开发者反馈要在 Ubuntu 22.04 上做。
- 部分 ONNX 算子转换后可能不支持。
- NPU 的具体设备选择和配置，要查官方的 Native 指南并在真机上验证。

## 6. 一致性验证

### 为什么结果会不一样

同一个模型、同一个输入，两个引擎的输出一般不是逐位相同的。原因前面都遇到过：

| 原因 | 在哪一课见过 | 量级 |
|---|---|---|
| 浮点加法的顺序不同（SIMD、多线程、分块） | 第 5 课 | 很小，1e-6 上下 |
| 图优化改变了计算方式（合并 BatchNorm） | 第 3 课；本课第 3 节的 9.5e-07 | 很小 |
| NPU 用半精度（FP16）计算 | 本课示例 | 1e-4～1e-3 |
| 量化 | 第 6 课 | 更大，要用精度指标衡量 |
| **预处理不一样** | — | 可以任意大 |

最后一项不是引擎的问题，却是实际中最常见的原因：两边的缩放方法不同、通道顺序一个是 BGR 一个是 RGB（OpenCV 教程第 2 课）、归一化用的均值和方差不同。**做一致性验证时，喂给两个引擎的必须是同一份已经预处理好的数据。**

### 怎么做

POC-4 的通过标准是“同一张图两端输出的最大误差 < 1e-3”（[POC 计划](../../../04-poc-plan.md)）。做法就是示例里的 `compare`：

```cpp
double compare(InferBackend& a, InferBackend& b, const Tensor& input) {
    return inftut::maxAbsDiff(a.infer(input), b.infer(input));
}
```

示例里的假 NPU 把每个算子的结果都截成 FP16 的精度（尾数只留 10 位），用来模拟半精度计算。模型 A 的最大误差是 5.73e-04，在阈值以内，但离阈值已经不远。

这里有一个要想清楚的地方：**FP16 只有 3 位多的有效数字。** 输出值在 1 上下时，误差在 1e-3 以内是做得到的；输出值是几十、几百时，绝对误差 1e-3 就是不可能达到的要求。所以实际定标准时，通常同时看：

- **相对误差**，或者按输出的量级换算过的误差；
- **最终的判定有没有变**：分类模型看类别是否相同；异常检测看 OK/NG 的判定在基准图集上是否一致。这才是产线关心的。

### 不通过时怎么查

1. 先排除预处理：把送进引擎的输入张量保存下来，两边对比。
2. **逐层对比。** 让两个引擎都把中间结果输出来，从前往后找到第一个差别变大的算子。问题就在那个算子，或者它的参数上。
3. 把那个算子单独拿出来，构造一个只含它的小模型来复现。

第 2 步和“给执行引擎的每个节点记录耗时”是同一种设计：**事先留好观察中间结果的手段。**

## 7. 预热

示例里，同一个模型连续推理 4 次：

```
预热：同一个模型连续推理 4 次的耗时
  30.41 ms 0.28 ms 0.22 ms 0.22 ms
```

这里的 30 ms 是示例用 `sleep` 模拟的，但现象是真的。上面 ONNX Runtime 的实测也一样：1.91 ms，之后是 0.4～0.5 ms。

第一次慢的原因：很多准备工作被推迟到了第一次推理时才做。内存池第一次申请内存；代码和权重第一次被读进缓存；用到 GPU、NPU 时，还要给硬件编译子图、把权重搬到设备上，这一项可能长达几百毫秒甚至几秒。

对策就是**预热**（[术语表](../../../ddd/02-glossary.md)：“启动后先空跑几次推理，避免第一次检测过慢”）：加载模型之后，先用一份形状正确的假数据推理几次，再对外宣布“可以检测了”。

- 工位启动时：加载上次生效的上线包，并预热（热启动）。
- 热切换时：新模型在后台加载**并预热完成**之后，才切换过去。否则切换后的第一个零件会碰上最慢的那一次推理，超出节拍。

## 8. 和项目的关系

| 这一课的内容 | 项目里对应什么 |
|---|---|
| `InferBackend` 接口 | 架构文档里的端口，ADR-11；还没有实现 |
| ONNX Runtime 适配器 | `adapters/` 下将来的一个目录，和 `imaging`、`persistence` 并列 |
| MindSpore Lite 适配器 | 鸿蒙端的适配器，POC-4 的真机部分 |
| 子图划分和回退 | 引擎内部做的，项目不用写；但要会读它的结果，并在选模型时考虑它 |
| 一致性验证 | POC-4 的通过标准 |
| 预热 | 术语表里的 `Warmup`；热启动、热切换（POC-9） |
| 推理线程 | C++ 教程第 16 课的推理服务，就是包在 `InferBackend` 外面的那一层 |

## 9. 动手运行

用 CMake（在 `docs/tutorial/inference` 目录下）：

```bash
cmake --build --preset default --target l07_backend
```

```bash
./build/bin/l07_backend
```

直接调用编译器（在本课的文件夹里）。**macOS / Linux**：

```bash
clang++ -std=c++17 -I../common backend.cpp -o backend -pthread && ./backend
```

**Windows**：

```bat
cl /std:c++17 /EHsc /utf-8 /I..\common backend.cpp && backend.exe
```

可选的 Python 示例（不在自动构建里）：

```bash
pip install onnx onnxruntime numpy
```

```bash
python ort_demo.py
```

## 10. 练习

1. **算一算临界点**：模型 B 在示例假设的耗时下，“NPU + 回退”比纯 CPU 慢。把 `kSwitchCostMs` 改小，切换一次的耗时降到多少以下，用 NPU 才划得来？再把模型 B 的第二个 `HardSwish` 换成 `Relu`，结果怎么变？
2. **替换算子**：`HardSwish(x) = x × clamp(x + 3, 0, 6) / 6`。假设 NPU 支持乘法和一个叫 `Relu6` 的算子（`clamp(x, 0, 6)`），给示例加上 `Relu6`、`AddConst`、`Mul` 这样的算子，把模型 B 里的 `HardSwish` 拆成几个受支持的算子。这样划分结果是几段？输出和原来一致吗？
3. **逐层对比**：给 `EngineBackend` 加一个函数，返回每个算子之后的中间结果。写一个函数，对比两个后端的中间结果，打印每一层的最大误差。误差是逐层变大的吗？
4. **换一种误差标准**：把一致性比较从“最大绝对误差”改成“最大相对误差”（除以参考值的绝对值，注意参考值接近 0 的情况）。两种标准下，模型 A 的结论一样吗？把输入的范围从 ±1 改成 ±100 再试一次。
5. **读一读真的结果**（需要安装 Python 环境）：运行 `ort_demo.py`。把优化级别换成 `ORT_ENABLE_ALL`，看看节点列表变成了什么，以及 ONNX Runtime 给出的警告。

## 11. 自测题

1. 为什么要定义 `InferBackend` 接口，而不是在算子里直接调用 ONNX Runtime？
2. 为什么 `InferBackend` 的接口里不能出现 `Ort::Value` 这样的类型？
3. ONNX Runtime 的 `CreateTensor` 不复制数据，这带来什么好处和什么责任？
4. 什么是 execution provider？为什么总是把 CPU 放在列表的最后？
5. 什么是子图划分？什么是 CPU 回退？
6. 模型里只有一两个算子不被 NPU 支持，为什么可能比完全不用 NPU 还慢？
7. 同一个模型在两个引擎上的输出为什么不会逐位相同？举出三个原因。
8. 一致性验证不通过时，按什么顺序排查？
9. 为什么“最大绝对误差 < 1e-3”这个标准对 FP16 的 NPU 不一定合适？
10. 第一次推理为什么慢？项目里在哪两个时刻需要预热？

## 下一课

第一部分（推理运行时）到这里结束。第二部分转向大模型。

[第 8 课：Transformer：手写一个最小的 GPT 前向](../lesson08/README.md)
