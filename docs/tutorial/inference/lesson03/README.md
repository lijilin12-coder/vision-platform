# 第 3 课：图优化——常量折叠与算子融合

> 预计用时：2.5 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

第 2 课把网络变成了数据。这一课利用这一点：**在执行之前，先让程序把图改写一遍。**

1. 从训练框架导出的模型，为什么常常带着多余的东西？
2. 什么样的改写是允许的？怎么保证改完之后结果不变？
3. 推理引擎文档里的“graph optimization”“算子融合”，编译器里的“pass”，具体是什么？

## 1. 导出的图不是最适合执行的图

模型是为了训练方便而写的，导出时只是把训练代码里的每一步原样记下来。于是图里会留下很多对推理没有用、或者可以提前算好的东西。这一课的示例（[`optimize_demo.cpp`](optimize_demo.cpp) 的 `buildExportedModel`）在第 2 课的网络上加了两类最常见的：

```cpp
// (1) 预处理被原样写进了图里：norm = input + (negMean + shift)。
//     negMean 和 shift 都是常量，它们的和现在就能算出来。
g.nodes.push_back(constantNode("neg_mean", ...));
g.nodes.push_back(constantNode("shift", ...));
g.nodes.push_back(simpleNode("offset", OpType::Add, {"neg_mean", "shift"}));
g.nodes.push_back(simpleNode("norm", OpType::Add, {"input", "offset"}));

// (2) 训练时用的辅助输出没有删掉：一个卷积加一个 ReLU，结果没有人用。
addConv(g, rng, "aux_conv", "pool1", channels, channels);
g.nodes.push_back(simpleNode("aux_relu", OpType::Relu, {"aux_conv"}));
```

再加上原来就有的 4 个 `BatchNorm`——它在训练时很重要，但推理时只是一次“乘一个数、加一个数”，完全可以并到前面的卷积里去。

第 2 课的执行器会老老实实地把这 25 个节点全部执行一遍，包括那个没人用的卷积。

## 2. 遍：读入一张图，输出一张等价的图

改写图的一个步骤叫一个**遍**（pass）。这个词来自编译器：编译器把你的 C++ 代码变成机器码的过程，就是几十个遍依次改写同一份中间表示，`-O2` 无非是打开了其中的一批。

每个遍只做一件小事，但必须遵守一条规则：

> **改写前后，对任何输入，图的输出都相同。**

（“相同”在浮点数上要打一点折扣，第 6 节会说。）

这一课实现四个遍，都在 [`passes.h`](passes.h) 里。每个遍的签名都是 `int pass(Graph&)`，返回它改动了几处。

## 3. 遍 1：删除无用节点

**判断标准**：从输出节点出发，沿着输入往回走，走得到的节点才对输出有贡献。走不到的都可以删。

```cpp
inline int removeDeadNodes(Graph& g) {
    std::map<std::string, bool> alive;
    std::vector<std::string> todo = {g.output};
    while (!todo.empty()) {
        const std::string name = todo.back();
        todo.pop_back();
        if (alive[name]) continue;
        alive[name] = true;
        const std::size_t i = findNode(g, name);
        if (i == g.nodes.size()) continue;
        for (const std::string& in : g.nodes[i].inputs) todo.push_back(in);
    }
    // 只保留 alive 的节点（Input 总是保留）
    ...
}
```

这是从输出往回的一次图遍历。注意方向：不是“没有下游的节点就删”，那样只能删掉 `aux_relu`，删完之后 `aux_conv` 才变成没有下游，要反复好几轮。从输出往回走，一次就能找全。

## 4. 遍 2：常量折叠

**判断标准**：一个节点的输入如果全是常量，那它的输出也是常量——不管输入图像是什么，它每次算出来都一样。那就现在算一次，把结果存下来。

```cpp
inline int foldConstants(Graph& g) {
    int folded = 0;
    for (Node& node : g.nodes) {
        if (node.op == OpType::Input || node.op == OpType::Constant) continue;
        std::vector<const Tensor*> in;
        for (const std::string& name : node.inputs) {
            const std::size_t i = findNode(g, name);
            if (i == g.nodes.size() || g.nodes[i].op != OpType::Constant) break;
            in.push_back(&g.nodes[i].weights[0]);
        }
        if (in.size() != node.inputs.size()) continue;  // 有的输入不是常量

        Tensor value;
        runNode(node, in, value);  // 用和推理时完全相同的代码来算
        node = constantNode(node.name, std::move(value));
        ++folded;
    }
    return folded;
}
```

两点值得注意：

- **折叠时调用的就是 `runNode`**，和推理时是同一份代码。这样折叠出的值和“推理时现算”的值逐位相同。
- 它只把 `offset` 换成了一个常量节点，**没有去删** `neg_mean` 和 `shift`。它们现在没人用了，下一次运行“删除无用节点”时自然会被删掉。每个遍只管自己的事，清理工作交给别的遍，这是遍的通常写法。

C++ 编译器对你的代码做同样的事：`const int n = 3 * 1024;` 编译出来直接就是 3072。

在真实的导出模型里，常量折叠最大的用处是清理**形状计算**。PyTorch 里一句 `x.view(x.size(0), -1)`，导出后会变成“取形状、取第 0 个元素、拼接、Reshape”一串节点，而输入大小固定时它们全是常量。

## 5. 遍 3：Conv + BatchNorm 融合

这是最经典的一个融合，值得把推导看懂。

**BatchNorm 在推理时做什么。** 对每个通道 c，有四个训练时得到的常数 γ、β、μ、σ²，计算

```
y = γ · (x − μ) / √(σ² + ε) + β
```

把只和常数有关的部分先算出来，令 `s = γ / √(σ² + ε)`、`t = β − μ·s`，它就是

```
y = s · x + t
```

每个通道乘一个数、加一个数。（训练时它还要统计当前这批数据的均值和方差，所以训练时不能这样简化。）

**卷积做什么。** 第 c 个输出通道的每个元素是

```
x = Σ (w · 输入) + b
```

**代进去：**

```
y = s · (Σ (w · 输入) + b) + t
  = Σ ((s·w) · 输入) + (s·b + t)
```

结果还是一个卷积，只是**权重变成 s·w，偏置变成 s·b + t**。所以只要在加载模型时把卷积的权重改掉，`BatchNorm` 节点就可以整个删除，推理时一点额外的计算都没有。

```cpp
for (int oc = 0; oc < weight.n; ++oc) {
    const float s = bn.weights[0].data[ch] / std::sqrt(bn.weights[3].data[ch] + kBatchNormEps);
    const float t = bn.weights[1].data[ch] - bn.weights[2].data[ch] * s;
    for (std::size_t i = 0; i < perChannel; ++i) weight.data[ch * perChannel + i] *= s;  // 这个输出通道的整组核都乘 s
    bias.data[ch] = bias.data[ch] * s + t;
}
redirect(g, bn.name, convName);   // 原来用 BatchNorm 输出的地方，改成直接用卷积的输出
g.nodes.erase(...);               // 删除 BatchNorm 节点
```

**什么时候不能融合。** 代码里有这样一个条件：

```cpp
if (useCount(g, g.nodes[c].name) != 1) continue;
```

如果卷积的输出除了给 `BatchNorm`，还被别的节点使用，就不能改它的权重——那个节点要的是改之前的结果。**每个遍都有这样的“合法性条件”，漏掉它，模型就会在某些结构上悄悄算错。** 写遍的难处大多在这里，而不在改写本身。

## 6. 遍 4：Conv + ReLU 融合

`ReLU` 没有参数，没法并进权重。这里融合的是**计算过程**：卷积每算出一个输出元素，顺手把负数改成 0。

```cpp
// conv2dInto 里：
if (relu && sum < 0) sum = 0;
y.at(n, oc, oy, ox) = sum;
```

遍本身只是把卷积节点的 `relu` 属性设成 `true`，然后删掉 `ReLU` 节点。省下的是：

- 对整个张量的第二次遍历。卷积刚写完的数据要再读一遍、再写一遍；张量大到缓存放不下时，这一遍的代价主要是访问内存（C++ 教程第 14 课）。
- 一个中间张量。原来 `conv` 的输出和 `relu` 的输出是两块内存，现在只要一块（第 4 课）。

这两种融合代表了两类做法：**Conv+BN 是“把计算提前到加载时做完”，Conv+ReLU 是“把两次遍历并成一次”。** 引擎里其他的融合基本都属于这两类之一。

## 7. 遍管理器：反复运行，直到没有可改的

```cpp
inline void optimize(Graph& g, ...) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Pass& pass : allPasses()) {
            const int changes = pass.apply(g);
            if (changes == 0) continue;
            changed = true;
            ...
        }
    }
}
```

为什么要循环？因为**一个遍的结果会给另一个遍创造机会**：

- 常量折叠之后，原来的 `neg_mean` 和 `shift` 才变成无用节点；
- 原图里是 `Conv → BatchNorm → ReLU`，卷积和 ReLU 并不相邻。融合掉 BatchNorm 之后它们才挨在一起，Conv+ReLU 融合才有机会。

反过来，顺序排得不好也会**堵死**机会：如果 Conv+ReLU 先于 Conv+BN 运行，并且图里有 `Conv → ReLU → BatchNorm` 这种结构，卷积先带上了 ReLU，后面的 BN 就不能再用上面的公式合并进去了（代码里的 `if (g.nodes[c].relu) continue;`）。遍的先后顺序是编译器里一个出名的难题。

## 8. 运行结果

```
输入 64x64，第一层 16 个通道

优化前：25 个节点（Add 3，BatchNorm 4，Constant 2，Conv 5，Input 1，Linear 1，MaxPool 3，Relu 5，Softmax 1）
  改动 2 处，剩 23 个节点，输出的最大变化 0        ← 删除无用节点
  改动 1 处，剩 23 个节点，输出的最大变化 0        ← 常量折叠
  改动 4 处，剩 19 个节点，输出的最大变化 4.2e-07  ← Conv+BatchNorm 融合
  改动 3 处，剩 16 个节点，输出的最大变化 4.2e-07  ← Conv+ReLU 融合
  改动 2 处，剩 14 个节点，输出的最大变化 4.2e-07  ← 删除无用节点
优化后：14 个节点（Add 2，Constant 1，Conv 1，Conv+ReLU 3，Input 1，Linear 1，MaxPool 3，Relu 1，Softmax 1）

优化后的执行顺序：input offset norm conv1(+relu) pool1 conv2a(+relu) conv2b add2 relu2 pool2 conv3(+relu) pool3 fc prob
```

25 个节点变成 14 个。“删除无用节点”运行了两次：第二次清理的是常量折叠留下的两个常量。还剩一个 `Relu`（`relu2`）：它前面是 `Add` 而不是卷积，这个遍处理不了。

### 怎么保证没有改错

示例里，**每个遍改动之后都立刻重新编译、执行一次，和优化前的输出比较**：

```cpp
optimize(optimized, [&](const Pass& pass, int changes, const Graph& g) {
    const double diff = maxAbsDiff(run(mustCompile(g), image).data, expected.data);
    ...
    ok = ok && diff < 1e-5;
});
```

前两个遍的差距是 0：删除节点和常量折叠没有改变任何一步计算。

Conv+BN 融合之后差距是 4.2e-07，不是 0。这**不是 bug**。`s·(Σw·x + b) + t` 和 `Σ(s·w)·x + (s·b + t)` 在数学上相等，但浮点数的每一步运算都有舍入，运算顺序变了，舍入的位置就变了。`float` 大约有 7 位有效数字，输出是 0～1 之间的概率，差在第 7 位是正常的。

所以验证图优化（以及后面所有的优化）时，判断标准是“**差距在浮点误差的范围内**”，而不是“逐位相同”。差距到了 1e-3 这个量级，就该怀疑是真的改错了。这也是 POC-4（两个推理引擎的一致性）要用的方法。

### 快了多少

```
按算子类型统计的耗时（毫秒，每一步重复 15 次取中位数）：
                before     after
  Add            0.062     0.036
  BatchNorm      0.178     0.000
  Constant       0.008     0.003
  Conv          15.691    12.361
  Input          0.005     0.003
  Linear         0.017     0.014
  MaxPool        0.114     0.113
  Relu           0.452     0.049
  Softmax        0.001     0.001
  total         16.527    12.580   （快了 23.9%）
```

（数字来自一台 2.1 GHz 的机器，你的结果会不同，看比例。）

总共省了约 3.9 ms。按第 1 课的习惯，看清楚它是从哪里省出来的：

| 来源 | 省了多少 | 是哪个遍的功劳 |
|---|---|---|
| Conv：15.69 → 12.36 ms | 约 3.3 ms | 删除无用节点（少算了一个没人用的卷积） |
| BatchNorm：0.18 → 0 ms | 约 0.2 ms | Conv+BN 融合 |
| Relu：0.45 → 0.05 ms | 约 0.4 ms | Conv+ReLU 融合 |

**大头来自最不起眼的那个遍。** 两个融合加起来只省了 0.6 ms，不到总时间的 4%。（严格地说还要更少：ReLU 的那次比较并没有消失，而是挪进了卷积里，算在 Conv 那一行。）原因在第 1 课就看到了：我们的朴素卷积占了 95% 的时间，其他算子再怎么省也只是零头。等卷积本身被优化之后（第 5 课），这些零头的占比才会上升。

这个结果本身就是一条经验：**融合的收益取决于被融合掉的东西原来占多少。** 不要因为“引擎都做算子融合”就默认它一定有大收益，要测。

## 9. 真实的引擎里

**ONNX Runtime** 把这些遍分成几个级别，创建会话时选择：

| 级别 | 做什么 | 这一课对应的 |
|---|---|---|
| `ORT_DISABLE_ALL` | 不优化 | 第 2 课的执行器 |
| `ORT_ENABLE_BASIC` | 不改变语义、对所有硬件都适用的改写：常量折叠、删除多余节点、Conv+BatchNorm 等融合 | 遍 1、2、3 |
| `ORT_ENABLE_EXTENDED` | 更复杂的融合，其中有的只对特定的后端有效：Conv+激活函数，以及 Transformer 里的注意力、LayerNorm、GELU 融合 | 遍 4 |
| `ORT_ENABLE_ALL` | 再加上和内存排列有关的优化 | |

还可以让它把优化之后的图存成文件：

```cpp
Ort::SessionOptions options;
options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
options.SetOptimizedModelFilePath("model.optimized.onnx");
```

用 Netron 分别打开优化前后的两个文件，就能看到引擎对你的模型做了什么。做 POC-4 时建议实际做一次——这是理解一个推理引擎最快的办法之一。

**AI 编译器**（TVM、MLIR）把同样的想法做得更彻底：图不只是被改写，还会一层层地翻译成更接近硬件的表示，每一层上都有一批遍。岗位要求里的“熟悉 MLIR/TVM”，入口就是这一课的内容：一种表示（IR）、一批遍、每个遍的合法性条件、一个遍管理器。

**对 NPU 来说融合更重要。** 很多 NPU 的硬件里直接有“卷积 + 激活”这样的复合单元，图里如果是分开的两个节点，要么用不上，要么中间多一次数据搬运。所以适配一种新芯片时，“针对它该做哪些融合”是软件栈的主要工作之一。

## 10. 和项目的关系

平台的方案图可以做一模一样的事，目前都还没有做。

**删除无用节点。** 用户在画布上拖了一个节点、没有连到任何判定上，现在执行引擎照样会执行它。在 `ExecutionPlanner::plan()` 里从判定节点往回走一遍，就能跳过它。

**算子融合。** 最常见的方案开头是 `to_gray → threshold`。现在是两次遍历 500 万个像素：第一次算出灰度图写进内存，第二次读出来和阈值比较。融合之后是一次遍历：算出一个像素的灰度，立刻比较，直接写出结果，灰度图这个中间结果根本不用存在。这就是这一课的 Conv+ReLU 融合。

但融合在项目里有一个模型里没有的**代价**：融合之后中间结果不存在了。调试方案时（`vision-cli recipe debug`）用户要看每个节点的输出图像，融合掉的节点就没有图可看。所以如果做，应该是“运行模式融合、调试模式不融合”。ONNX Runtime 里也是同样的情况：优化之后，原图里的一些张量在执行时已经不存在了，想取出来看就得关掉优化。

另外按 C++ 教程第 14 课的原则：POC-1 实测整个流程约 8 ms，远低于 100 ms 的要求，**现在没有理由做这件事**。这里要知道的是“可以做、怎么做、代价是什么”。

## 11. 动手运行

用 CMake（在 `docs/tutorial/inference` 下）：

```bash
cmake --build --preset default --target l03_optimize_demo l03_optimize_bench
```

```bash
./build/bin/l03_optimize_demo
```

```bash
./build/bin/l03_optimize_bench 64 16
```

两个程序是同一份源文件：`l03_optimize_demo` 带内存检查、不开优化，用来看对不对；`l03_optimize_bench` 打开了优化，用来看耗时。两个参数是图像边长和第一层的通道数。

或者直接调用编译器（在本文件夹下）。**macOS / Linux**：

```bash
clang++ -std=c++17 -O2 -I../common optimize_demo.cpp -o optimize_demo && ./optimize_demo 64 16
```

**Windows**：

```bat
cl /std:c++17 /O2 /EHsc /utf-8 /I..\common optimize_demo.cpp && optimize_demo.exe 64 16
```

## 12. 练习

1. **去掉合法性条件**：把 `fuseConvBatchNorm` 里的 `if (useCount(...) != 1) continue;` 删掉。示例还能通过吗？然后在 `buildExportedModel` 里把 `add2` 的第二个输入从 `pool1` 改成 `conv2a`（形状相同），这样 `conv2a` 的输出就有两个使用者：`bn2a` 和 `add2`。现在呢？哪个遍之后输出开始不对，差距是多少？（做完记得把条件加回去；示例最后对节点个数的检查会因为图变了而失败，可以先注释掉。）
2. **颠倒遍的顺序**：在 `allPasses()` 里把“Conv+ReLU 融合”挪到“Conv+BatchNorm 融合”前面。对这个示例网络，结果有变化吗？为什么？再构造一个 `Conv → ReLU → BatchNorm` 的图，看看两种顺序下各剩几个节点。
3. **写一个新的遍**：`Add` 的一个输入是常量、另一个输入是带偏置的卷积时（示例里没有这种结构，自己构造一个 `Conv → Add(常量)`，常量是每个通道一个数），可以把常量并进卷积的偏置。写出这个遍，写明它的合法性条件，并验证。
4. **去掉最后一个 ReLU**：`relu2` 前面是 `Add`，没法并进卷积。给 `Add` 节点也加上 `relu` 属性（需要改 `runNode` 和 `addInto`），写一个 Add+ReLU 融合的遍。节点数变成多少？耗时有可见的变化吗？
5. **搬到项目里**：读 [`plan.cpp`](../../../../core/contexts/execution/domain/src/plan.cpp)。如果要在 `ExecutionPlanner::plan()` 里加“删除无用节点”，“有用”应该怎么定义？（提示：方案的输出是什么？看 `executor.cpp` 里是怎么收集判定依据的。）只写出设计，不用实现。

## 13. 自测题

1. 为什么导出的模型里会有无用节点和可以提前算好的节点？
2. 一个遍必须遵守的规则是什么？
3. 删除无用节点时，为什么从输出往回走，而不是找“没有下游的节点”？
4. 写出 Conv+BatchNorm 融合后新的权重和偏置的公式。为什么训练时不能做这个融合？
5. Conv+BN 融合和 Conv+ReLU 融合，省下的东西有什么不同？
6. “卷积的输出只有一个使用者”这个条件如果不检查，会出什么问题？
7. 为什么遍要反复运行？举一个“一个遍给另一个遍创造机会”的例子。
8. 融合之后输出和原来差了 4e-07，是不是 bug？差了 4e-03 呢？
9. 示例里省下的时间主要来自哪个遍？这说明了什么？
10. 在检测方案里做算子融合，有什么模型里没有的代价？

## 下一课

[第 4 课：内存规划——张量生命周期与缓冲区复用](../lesson04/README.md)
