# 第 2 课：计算图与执行计划

> 预计用时：2 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

第 1 课的网络结构写死在 `forward` 函数里。推理引擎显然不能这样：它要能加载任意一个模型文件。

1. 怎么把“网络长什么样”表示成数据？
2. 拿到这份数据之后，怎么知道先算哪个、后算哪个？
3. 哪些错误应该在加载模型时就发现，而不是等到推理时？

这三个问题，平台的执行引擎已经回答过一遍了——针对的是检测方案。这一课会看到：**模型的图和方案的图是同一种结构，处理它们的代码也几乎一样。**

## 1. 写死的 `forward` 有什么问题

```cpp
Tensor forward(const Weights& w, const Tensor& image) {
    Tensor x = inftut::conv2d(image, w.conv1W, w.conv1B, 1, 1);
    x = inftut::relu(x);
    ...
}
```

- **换一个模型就要改代码、重新编译。** 产线上换模型应该只是换一个文件。
- **程序没法“看见”网络的结构。** 想统计有几个卷积、想把相邻的两个算子合并、想知道哪个张量什么时候不再需要——这些信息都埋在 C++ 代码里，程序自己读不到。后面两课要做的优化全都需要这些信息。

解决办法和平台对检测方案的做法相同：**把结构变成数据。** 平台没有把“灰度 → 阈值 → Blob → 判定”写成一个函数，而是存成一份方案（节点和连线），由执行引擎读出来执行。

## 2. 图：节点和它们之间的连接

这一课换一个稍微复杂一点的网络（[`model.h`](model.h)），它不是一条直线：

```
input ─ conv1 ─ bn1 ─ relu1 ─ pool1 ─┬─ conv2a ─ bn2a ─ relu2a ─ conv2b ─ bn2b ─┐
                                     │                                         add2 ─ relu2 ─ pool2 ─ ...
                                     └─────────────────────────────────────────┘
... ─ conv3 ─ bn3 ─ relu3 ─ pool3 ─ fc ─ prob
```

`pool1` 的输出被两个地方使用：一路经过两层卷积，另一路直接跳过去，最后两路相加（`add2`）。这叫**跳跃连接**，是 ResNet 的核心结构，现在的网络（包括大语言模型）里到处都是。有了它，网络就不能再用“一条链”描述，只能用**图**。

（这里多了两个算子：`Add` 是逐元素相加；`BatchNorm`——批归一化——对每个通道做一次“乘一个数、加一个数”，它的细节留到第 3 课。）

[`graph.h`](graph.h) 里图的定义：

```cpp
struct Node {
    std::string name;                 // 节点的名字，也是它输出的张量的名字
    OpType op;                        // 哪种算子：Conv、Relu、Add……
    std::vector<std::string> inputs;  // 输入来自哪些节点

    int stride = 1, pad = 0, kernel = 2;  // 属性：算子的参数里不是张量的那部分
    std::vector<Tensor> weights;          // 权重：算子的参数里是张量的那部分
};

struct Graph {
    std::vector<Node> nodes;
    std::string output;  // 哪个节点的输出是整个模型的输出
};
```

一个节点就是**一次算子调用的描述**：调用哪个算子、输入从哪里来、参数是什么。把上一课 `forward` 里的每一行变成一个 `Node`，放进数组，就得到了图：

```cpp
g.nodes.push_back(inputNode("input", Shape{1, 3, 32, 32}));
addConv(g, rng, "conv1", "input", 3, c);                           // conv1 的输入是 input
addBatchNorm(g, rng, "bn1", "conv1", c);                           // bn1 的输入是 conv1
g.nodes.push_back(simpleNode("relu1", OpType::Relu, {"bn1"}));
g.nodes.push_back(maxPoolNode("pool1", "relu1", 2));
...
g.nodes.push_back(simpleNode("add2", OpType::Add, {"bn2b", "pool1"}));  // 两个输入
```

连接关系没有单独存放，而是写在每个节点的 `inputs` 里：“我的输入是谁”。

注意 `Graph` 里的节点**没有顺序**的含义。`nodes` 是个数组只是为了存放方便，把它打乱，表示的还是同一个网络。

### 和 ONNX 对照

ONNX 是模型文件最通用的格式，PyTorch 训练出的模型一般先导出成它。一个 `.onnx` 文件里的结构和上面几乎一一对应：

| 这一课 | ONNX 文件里 | 说明 |
|---|---|---|
| `Graph` | `GraphProto` | |
| `Node` | `NodeProto` | |
| `Node::op` | `op_type`（字符串，如 `"Conv"`） | |
| `Node::inputs` | `input`：输入张量的名字 | ONNX 的节点可以有多个输出，所以连接是按**张量的名字**而不是节点的名字。这一课每个节点只有一个输出，两者就合并了 |
| `stride`、`pad` 等 | `attribute` | |
| `Node::weights` | `initializer`：图里所有的常数张量 | ONNX 把权重集中放在图上，卷积通过名字引用它 |
| `Graph::output` | `output` | |

想看一个真实模型的图，用 [Netron](https://netron.app)：把 `.onnx` 文件拖进去，就能看到每个节点、它的属性和权重的形状。接入任何模型之前都值得先这样看一眼。

## 3. 从图到执行计划

图只说了“谁依赖谁”，没有说“先算谁”。执行之前要做三件事，合起来叫**编译**（`compile` 函数）。

### 第一件：排出执行顺序

规则只有一条：**一个节点的所有输入都算出来之后，才能算它。** 满足这条规则的顺序叫**拓扑顺序**，求它的过程叫拓扑排序。你在算法课上学过，这里用的是 Kahn 算法：

```cpp
// 数出每个节点有几个还没算出来的输入（入度）
// 反复取出入度为 0 的节点；取出一个，就把依赖它的节点的入度减 1
std::deque<std::size_t> ready;
for (std::size_t i = 0; i < count; ++i)
    if (indegree[i] == 0) ready.push_back(i);
std::vector<std::size_t> order;
while (!ready.empty()) {
    const std::size_t cur = ready.front();
    ready.pop_front();
    order.push_back(cur);
    for (std::size_t nx : next[cur])
        if (--indegree[nx] == 0) ready.push_back(nx);
}
if (order.size() != count) return fail("图里有环，排不出执行顺序");
```

最后一行顺便检查了环：如果有节点永远等不到入度变成 0，说明它们互相依赖。

### 第二件：把名字换成下标

图里的连接是字符串（“我的输入是 `pool1`”）。每次推理都按名字去查太慢，所以编译时就把它换成“第几步的输出”：

```cpp
struct Step {
    Node node;                        // 这一步执行哪个节点
    std::vector<std::size_t> inputs;  // 每个输入来自第几步的输出（一定比自己的下标小）
    Shape shape;                      // 这一步输出的形状
};
```

### 第三件：推导形状

卷积的输出多大，取决于输入多大；输入多大，取决于再上一个节点。沿着执行顺序走一遍，就能算出每个张量的形状，这叫**形状推导**（shape inference）：

```cpp
case OpType::Conv: {
    const Tensor& w = node.weights[0];
    if (w.c != in[0].c) return fail("卷积核要求 " + ... + " 个输入通道，输入是 " + in[0].text());
    const int oh = (in[0].h + 2 * node.pad - w.h) / node.stride + 1;
    const int ow = (in[0].w + 2 * node.pad - w.w) / node.stride + 1;
    out = Shape{in[0].n, w.n, oh, ow};
    return true;
}
case OpType::Add:
    if (in[0] != in[1]) return fail("两个输入的形状不同：" + in[0].text() + " 和 " + in[1].text());
    out = in[0];
    return true;
```

它有两个用处。一是**检查**：通道数对不上、两个要相加的张量大小不同，这些错误不用真的去算就能发现。二是**为后面做准备**：知道了每个张量多大，才能在推理之前把内存规划好（第 4 课）。

### 结果

运行 [`graph_demo.cpp`](graph_demo.cpp)，第一部分打印出编译得到的执行计划：

```
1. 执行计划（19 步）：
  步骤  节点      算子        输入来自        输出形状
  #0    input     Input                       1x3x32x32
  #1    conv1     Conv        #0              1x8x32x32
  #2    bn1       BatchNorm   #1              1x8x32x32
  #3    relu1     Relu        #2              1x8x32x32
  #4    pool1     MaxPool     #3              1x8x16x16
  #5    conv2a    Conv        #4              1x8x16x16
  #6    bn2a      BatchNorm   #5              1x8x16x16
  #7    relu2a    Relu        #6              1x8x16x16
  #8    conv2b    Conv        #7              1x8x16x16
  #9    bn2b      BatchNorm   #8              1x8x16x16
  #10   add2      Add         #9, #4          1x8x16x16
  #11   relu2     Relu        #10             1x8x16x16
  #12   pool2     MaxPool     #11             1x8x8x8
  #13   conv3     Conv        #12             1x16x8x8
  #14   bn3       BatchNorm   #13             1x16x8x8
  #15   relu3     Relu        #14             1x16x8x8
  #16   pool3     MaxPool     #15             1x16x4x4
  #17   fc        Linear      #16             1x10x1x1
  #18   prob      Softmax     #17             1x10x1x1
```

看第 10 步：`add2` 的两个输入来自第 9 步和第 4 步。第 4 步（`pool1`）的输出在第 5 步用过一次，一直要留到第 10 步再用一次。这个“一直要留着”是第 4 课的主题。

## 4. 执行

有了计划，执行就很简单：从头到尾走一遍，每一步取出输入、调用算子、存下输出。

```cpp
inline Tensor run(const Plan& plan, const Tensor& input) {
    std::vector<Tensor> values(plan.steps.size());  // values[i] 是第 i 步的输出
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        const Step& step = plan.steps[i];
        if (step.node.op == OpType::Input) {
            values[i] = input;
        } else if (step.node.op == OpType::Constant) {
            values[i] = step.node.weights[0];
        } else {
            std::vector<const Tensor*> in;
            for (std::size_t src : step.inputs) in.push_back(&values[src]);
            runNode(step.node, in, values[i]);   // 按算子类型调用第 1 课的函数
        }
    }
    return std::move(values[plan.outputStep]);
}
```

`runNode` 是一个 `switch`：`Conv` 就调用 `conv2dInto`，`Relu` 就调用 `reluInto`。真实的引擎里这里是一张“算子类型 → kernel”的注册表，和平台的 `VISION_REGISTER_OPERATOR` 是同一个办法（C++ 教程第 10 课）。

**编译只做一次，执行做很多次。** 检查、排序、查名字这些慢的事情都放在编译里；`run` 里只剩下一个循环。产线上一个模型加载一次，然后推理几百万次，这个划分很重要。

示例的后三部分验证了几件事：

```
2. 按计划执行的输出：0.099 0.050 0.196 0.121 0.044 0.112 0.267 0.030 0.036 0.044
   和手写前向计算的最大差距：0

3. 把节点倒过来放（第一个节点是 prob），排出的执行顺序：
   input conv1 bn1 relu1 pool1 conv2a bn2a relu2a conv2b bn2b add2 relu2 pool2 conv3 bn3 relu3 pool3 fc prob
   输出和原来的最大差距：0

4. 有问题的图，编译时报错：
  有环 → 图里有环，排不出执行顺序
  名字写错 → 节点 relu3 的输入 bn33 不存在
  形状对不上 → 节点 add2（Add）：两个输入的形状不同：1x8x16x16 和 1x8x32x32
```

- 第 2 部分：按计划执行，和像第 1 课那样手写前向计算（`forwardByHand`）的结果**逐位相同**。执行器只是换了一种方式调用同样的函数，不应该有任何差别。
- 第 3 部分：把 `nodes` 数组整个倒过来，编译出的执行顺序不变，结果不变。
- 第 4 部分：三种错误都在编译时报出来，而且消息里写明了是哪个节点、为什么。**模型有问题，要在加载时拒绝，不能等到产线上第一张图进来才崩溃。**

## 5. 项目里的例子：方案图和模型图是同一种结构

打开 [`plan.h`](../../../../core/contexts/execution/domain/include/vision/execution/domain/plan.h) 和 [`plan.cpp`](../../../../core/contexts/execution/domain/src/plan.cpp)，和这一课的 `graph.h` 对照：

| 这一课（模型） | 项目（检测方案） | 说明 |
|---|---|---|
| `Graph` | `PlanSpec` | 编译的输入：节点和连接 |
| `Node` | `PlanNode` | |
| `Node::inputs`（写在节点上） | `PlanEdge`（单独一个数组） | 两种存法，信息相同 |
| 属性、权重 | `ParamMap params` | |
| `compile()` | `ExecutionPlanner::plan()` | 检查、拓扑排序、绑定输入 |
| `Step` | `PlanStep` | |
| `Step::inputs`（第几步） | `InputBinding{sourceStep, sourcePort}` | 都是把名字换成了下标 |
| 形状推导 | 端口类型检查（`isCompatible`） | 都是“不用执行就能发现接错了” |
| `runNode()` 里的 `switch` | `NodeRunner`（由工厂按算子创建） | |
| `run()` | `Executor::run()` | 从头到尾走一遍步骤 |

`plan.cpp` 里的拓扑排序和这一课的是同一段代码：

```cpp
// Kahn 算法：反复取出入度为 0 的节点。保持原始顺序，使执行顺序稳定、可预期。
std::deque<size_t> ready;
for (size_t i = 0; i < n; ++i)
    if (indegree[i] == 0) ready.push_back(i);
```

“编译一次、执行多次”在项目里也有：`execution_service.cpp` 把编译好的执行计划按方案版本缓存在 `planCache` 里。

两边也有不同，都来自各自的需要：

- **端口。** 方案的算子有多个带名字的输入输出端口（`image`、`region`、`count`），所以连线要写明“从哪个端口到哪个端口”。这一课每个节点只有一个输出，省掉了。ONNX 的节点也可以有多个输出。
- **流动的数据。** 方案里流动的是 `Value`（图像、区域、数字、判定结果），类型各不相同，所以检查的是**类型**；模型里流动的全是张量，所以检查的是**形状**。
- **失败的处理。** 方案里一个节点失败了，下游节点标记为“跳过”，其余的照常执行并记录下来（`executor.cpp`），因为产线上要知道是哪一步出的问题。模型里一个算子失败，整次推理就失败。
- **规模。** 方案是几个到几十个节点，每个节点做的事情很重；模型是几十到几千个节点。所以模型这边更值得在“图”上做文章——下两课的内容。

**这意味着后面两课学的东西可以原样搬回项目。** 第 3 课的算子融合，对应“把 `to_gray` 和 `threshold` 合成一次遍历”；第 4 课的内存规划，对应“节点之间复用图像缓冲区”。它们在项目里都还没有做。

## 6. 动手运行

用 CMake（在 `docs/tutorial/inference` 下）：

```bash
cmake --build --preset default --target l02_graph_demo
```

```bash
./build/bin/l02_graph_demo
```

或者直接调用编译器（在本文件夹下）。**macOS / Linux**：

```bash
clang++ -std=c++17 -I../common graph_demo.cpp -o graph_demo && ./graph_demo
```

**Windows**：

```bat
cl /std:c++17 /EHsc /utf-8 /I..\common graph_demo.cpp && graph_demo.exe
```

## 7. 练习

1. **加一个节点**：在 `model.h` 的 `pool3` 和 `fc` 之间插入一个 `Relu` 节点。需要改哪几行？`graph_demo` 会在哪一项检查上失败，为什么？（提示：`forwardByHand` 也要跟着改。这正好说明了“结构写在代码里”的代价。）
2. **造一个新的错误**：把 `conv3` 的卷积核改成要求 4 个输入通道（把 `addConv(g, rng, "conv3", "pool2", c, 2 * c)` 的 `c` 换成 `4`）。编译时的错误消息是什么？它够不够让你定位问题？
3. **统计一个模型**：写一个函数 `void summarize(const Plan&)`，打印每种算子有几个、全部权重加起来有多少个数（参数量）、全部卷积一共要做多少次乘加。对示例网络运行它。这三个数分别决定了模型的什么？
4. **给每一步计时**：`run` 的第三个参数可以拿到每一步的耗时。用 `-O2` 编译，把网络的输入改成 64×64（`buildSmallResNet(64, 16)`），打印一张和第 1 课一样的耗时表。
5. **读项目代码**：`ExecutionPlanner::plan()` 在什么情况下返回 `execution.unbound_input`？这一课的 `compile` 里，对应的检查在哪里？

## 8. 自测题

1. 把网络结构写死在函数里，有哪两个问题？
2. 为什么带跳跃连接的网络必须用图来表示？
3. `Graph::nodes` 里节点的先后顺序有意义吗？执行顺序是怎么来的？
4. 编译做了哪三件事？为什么要把它和执行分开？
5. 形状推导有哪两个用处？
6. 执行计划里为什么用“第几步”而不是节点的名字来表示输入？
7. ONNX 文件里的 `initializer` 是什么？
8. 说出项目的执行计划和这一课的执行计划的两个相同点、两个不同点。

## 下一课

[第 3 课：图优化——常量折叠与算子融合](../lesson03/README.md)
