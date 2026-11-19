# 第 9 课：KV Cache：Prefill 与 Decode

> 预计用时：2 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起。本课的代码建立在 [第 8 课的 `tiny_gpt.h`](../lesson08/tiny_gpt.h) 上

## 这一课要回答的问题

上一课的结尾：不做优化时，生成第 n 个 token 要把前面 n 个 token 全部重新算一遍，连续生成 512 个 token 约 84 秒；如果每个 token 只算一次，只要 0.36 秒。

1. 哪些计算是重复的？要把什么存下来，才能不重算？
2. 存下来之后，一次请求变成了哪两个阶段？它们的性能特点有什么不同？
3. 首字延时（TTFT）和每 token 耗时（TPOT）是什么，怎么测？
4. 这份缓存要占多少内存？

KV Cache 是所有大模型推理引擎都有的基础优化，没有它的引擎不存在。后面三课（Continuous Batching、PagedAttention、投机解码）处理的都是“有了 KV Cache 之后”才出现的问题。

## 1. 重复的计算在哪里

上一课的示例验证过一个性质：**第 i 个位置在每一层的计算结果，只取决于第 0..i 个 token。** 后面再接多少个 token，前面的结果都不变。

看生成时连续的两步：

```
第 1 步：输入 [A B C]      算出 A、B、C 三个位置在每一层的结果，用 C 那一行预测出 D
第 2 步：输入 [A B C D]    算出 A、B、C、D 四个位置在每一层的结果，用 D 那一行预测出 E
                                 └──┬──┘
                          和第 1 步算出来的一模一样
```

第 2 步里，A、B、C 三个位置的全部计算都是白做的。**真正需要新算的只有 D 这一个位置。**

那么只算 D 这一行，需要用到前面位置的什么东西？回到上一课的结构，逐个看：

| 步骤 | 算 D 这一行时，要不要用到 A、B、C 的数据 |
|---|---|
| Embedding | 不用。只查 D 自己的 |
| 归一化 | 不用。每一行独立 |
| 算 D 的 q、k、v | 不用。只是 D 这一行乘矩阵 |
| **注意力：打分、加权求和** | **要。D 的 q 要和 A、B、C、D 的 k 做点积，再按权重把 A、B、C、D 的 v 加起来** |
| MLP | 不用。每一行独立 |
| 输出 logits | 不用 |

只有一处要用，而且用到的只是前面每个位置**在每一层的 K 向量和 V 向量**。前面位置的 Q 不需要（Q 只在它自己那个位置用一次），前面位置的中间结果也不需要。

所以做法是：**每个位置算完之后，把它在每一层的 K、V 存起来。** 这份存储就叫 **KV Cache**。之后每来一个新 token，只算这一个位置，注意力那一步去缓存里取前面位置的 K、V。

## 2. 实现

缓存的结构（[`kv_cache.h`](kv_cache.h)）：每一层两块连续的内存，各存 `length` 行、每行 `dim` 个数。

```cpp
struct KvCache {
    struct PerLayer {
        std::vector<float> keys;     // length × dim
        std::vector<float> values;   // length × dim
    };
    std::vector<PerLayer> layers;
    int dim = 0;
    int length = 0;   // 已经缓存了多少个位置
};
```

```
                位置 0   位置 1   位置 2   位置 3      ← 每来一个 token，每层各追加一行 K、一行 V
第 0 层 keys   [ k00  ] [ k01  ] [ k02  ] [ k03  ]
第 0 层 values [ v00  ] [ v01  ] [ v02  ] [ v03  ]
第 1 层 keys   [ k10  ] [ k11  ] [ k12  ] [ k13  ]
第 1 层 values [ v10  ] [ v11  ] [ v12  ] [ v13  ]
```

带缓存的注意力只有三步，和上一课的 `selfAttention` 对照着看：

```cpp
inline Mat selfAttentionCached(const Layer& layer, const Config& cfg, const Mat& x, KvCache::PerLayer& cache, int startPos) {
    // 1. 只给新 token 算 Q、K、V
    const Mat q = linear(x, layer.wq);
    const Mat k = linear(x, layer.wk);
    const Mat v = linear(x, layer.wv);
    // 2. 新的 K、V 追加到缓存末尾。Q 不用存：它只在自己这个位置用一次
    cache.keys.insert(cache.keys.end(), k.v.begin(), k.v.end());
    cache.values.insert(cache.values.end(), v.v.begin(), v.v.end());
    // 3. 每个新 token 去看缓存里从 0 到自己为止的全部位置
    Mat mixed(x.rows, cfg.dim);
    std::vector<float> scores(static_cast<std::size_t>(startPos + x.rows));
    for (int i = 0; i < x.rows; ++i) {
        attendOne(q.row(i), cache.keys.data(), cache.values.data(), startPos + i + 1, cfg.dim, cfg.heads, scores, mixed.row(i));
    }
    return linear(mixed, layer.wo);
}
```

这里的 `x` 只包含**新来的** token，可以是一个，也可以是多个。`forwardCached` 把它套在每一层外面，其余部分和上一课的 `forward` 相同，返回新 token 对应的那几行 logits。

两个实现上的细节：

- **位置要接着数。** 新 token 的位置是 `startPos = cache.length`，查位置表时要用这个值，不能从 0 开始。
- **缓存要预先留够空间。** `KvCache` 的构造函数按最大长度做了 `reserve`。否则每次追加都可能触发重新申请内存和搬运（C++ 教程第 14 课的“问题三”）。真实的引擎里这块内存怎么申请是一个大问题，[第 12 课](../lesson12/README.md) 专门讲。

## 3. 验证：结果必须完全一样

KV Cache 是纯粹的“少算”，不是近似。带缓存和不带缓存的结果应该完全相同。[`equivalence.cpp`](equivalence.cpp) 检查了这一点：

```
逐个 token 喂入，和不带缓存相比，logits 的最大误差：0
Prefill 8 个 + Decode 4 个，和不带缓存相比，logits 的最大误差：0

缓存了 12 个位置，占 6144 字节；公式算出来是 2 × 2 × 12 × 32 × 4 = 6144 字节
截断到 8 个位置后重新 Decode，最大误差：0

贪心生成 20 个 token，带缓存和不带缓存的结果相同：5 17 3 42 8 21 57 17 39 18 54 25 37 59 13 54 32 40 31 59 60 25 34 2 46 56
```

误差是 0，不是“很小”：两种算法对同样的数做了同样的运算，顺序也相同。（在个别编译器上可能出现最后一位的差别，所以程序里的判断留了 0.0001 的余量。）

**做任何推理优化都要先有这样一个检查。** 没有它，后面出现“输出有点不一样”的时候，你无法判断是优化写错了还是本来就该不一样。这和 C++ 教程第 14 课说的“优化之后要确认结果没变”是同一件事。

## 4. 一次请求的两个阶段

有了缓存，一次请求自然地分成两个阶段：

```
用户发来提示词（n 个 token）
        │
        ▼
┌─────────────────────────────┐
│ Prefill（预填充）             │  一次前向计算处理全部 n 个 token
│ 输入：n 个 token             │  → 缓存里有了 n 个位置的 K、V
│ 输出：第 1 个生成的 token      │  → 最后一行 logits 给出第 1 个输出 token
└─────────────────────────────┘
        │
        ▼
┌─────────────────────────────┐
│ Decode（解码）               │  每次前向计算只处理 1 个 token：上一步刚生成的那个
│ 输入：1 个 token             │  → 缓存加长一个位置
│ 输出：下一个 token           │  重复，直到结束
└─────────────────────────────┘
```

```cpp
inline std::vector<int> generateGreedyCached(const Model& m, std::vector<int> tokens, int newTokens) {
    KvCache cache(m.cfg);
    Mat logits = forwardCached(m, cache, tokens);                       // Prefill：一次处理整个提示词
    for (int i = 0; i < newTokens; ++i) {
        const int next = argmax(logits.row(logits.rows - 1), m.cfg.vocab);
        tokens.push_back(next);
        if (i + 1 < newTokens) logits = forwardCached(m, cache, {next});   // Decode：每次只处理一个 token
    }
    return tokens;
}
```

两个阶段各对应一个用户能感觉到的指标：

| 指标 | 全称 | 含义 | 主要由什么决定 |
|---|---|---|---|
| **TTFT** | time to first token，首字延时 | 从发出请求到看到第一个字 | Prefill 的耗时（线上还要加上排队的时间，第 11 课） |
| **TPOT** | time per output token，每 token 耗时 | 之后每出一个字要多久。它的倒数就是“每秒多少个 token” | Decode 一步的耗时 |

一次请求的总耗时 ≈ TTFT + TPOT ×（输出的 token 数 − 1）。

## 5. 测量

[`prefill_decode.cpp`](prefill_decode.cpp) 用上一课那个“隐藏维度 128、4 层”的模型测量。数字来自一台 2.1 GHz 的机器，单线程，你的结果会不同，看比例：

```
提示词 32 个 token，生成 48 个 token：
  不带缓存：  1466.5 ms
  带缓存：      49.8 ms（快 29 倍），生成的 token 完全相同

不同长度的提示词，各生成 32 个 token：
提示词长度      TTFT ms Prefill 每token ms      TPOT ms  KV Cache 字节
        16         8.25             0.5156       0.6387           196608
        64        34.97             0.5465       0.6791           393216
       256       169.51             0.6622       0.8874          1179648
       768       613.48             0.7988       1.2957          3276800

Decode 一步的耗时随上下文长度的变化：
上下文长度      TPOT ms
           1       0.6398
         250       0.7793
         500       1.0616
        1000       1.4925
```

怎么读这几张表：

- **带缓存快 29 倍，输出完全相同。** 生成得越长，差距越大：不带缓存的总时间和长度的平方成正比，带缓存的大致和长度成正比。
- **TTFT 和提示词长度成正比。** 提示词从 16 变成 768（48 倍），TTFT 从 8 毫秒变成 613 毫秒（74 倍，多出来的部分是注意力里和长度平方成正比的那一项）。提示词很长的应用（比如把整篇文档塞进去），用户等第一个字的时间主要花在 Prefill 上。
- **TPOT 几乎不随提示词长度变化，但不是完全不变。** 上下文从 1 个 token 到 1000 个，TPOT 从 0.64 涨到 1.49 毫秒。Decode 一步里，矩阵乘法那部分（12·d² 次乘法）和上下文长度无关；注意力那部分要和缓存里的每个位置各做一次点积，和上下文长度成正比。上下文越长，后者占比越大。

## 6. 两个阶段的计算特征不一样

Prefill 和 Decode 用的是同一份代码、同一组权重，但它们对硬件的要求很不一样。这是招聘要求里“Prefill 与 Decode 阶段的全链路性能分析”那句话的背景。

**Prefill：一次处理很多行。** 输入是 n × dim 的矩阵，每个线性层是一次“n 行”的矩阵乘法。n 是几百到几千时，这是一个很大的矩阵乘法，正是 [第 5 课](../lesson05/README.md) 那些方法（分块、SIMD、多线程）以及 GPU 最擅长的形状：权重矩阵读进缓存一次，可以连续用在很多行上；各行之间可以并行。这个阶段比的是**算力**。

**Decode：一次只处理一行。** 输入是 1 × dim，每个线性层退化成“矩阵乘一个向量”。为了产生一个 token，**全部权重都要从内存里读一遍**，每个权重只用一次。示例模型有 111 万个参数，约 4.4 MB；换成一个 70 亿参数、每个参数 2 字节的模型，就是每产生一个 token 要读 14 GB 的数据（7 × 10⁹ × 2 字节）。这时候算术运算本身不多，时间主要花在把权重从内存搬到处理器上，并行的余地也小。这个阶段比的是**内存带宽**，以及每一步之外的固定开销（调度、采样、框架的调用开销）。

在我们的示例里**看不出这个区别**，要如实说明：上面的表里，Prefill 平均每个 token 0.52 毫秒，Decode 每个 token 0.64 毫秒，相差不多。原因是示例的 `linear` 是最朴素的单线程循环，一行一行地算，没有利用“一次处理很多行”的机会；而且模型只有 4.4 MB，权重全部放得进 CPU 的缓存，不存在“搬权重”的开销。换成优化过的矩阵乘法、更大的模型或者 GPU，两者的差距会拉得很大：Prefill 时每个 token 的平均耗时远小于 Decode 一步的耗时。练习 3 让你用第 5 课的方法亲手验证一部分。

这个区别直接决定了后面几课的做法：

| 现象 | 后果 | 在哪一课 |
|---|---|---|
| Decode 一步只处理一行，硬件大部分时间在空等 | 把很多个请求的 Decode 拼在一起算，一次读权重服务很多请求 | [第 11 课](../lesson11/README.md)：Continuous Batching |
| 一次处理 k 行不比处理 1 行慢多少 | 先猜 k 个 token，再用一次前向计算验证 | [第 10 课](../lesson10/README.md)：投机解码 |
| Prefill 和 Decode 混在一起时互相拖累 | 调度器要决定先做谁，会不会让正在输出的请求卡顿 | 第 11 课 |

## 7. KV Cache 占多少内存

每个位置在每一层存一行 K、一行 V：

```
KV Cache 字节数 = 2 × 层数 × token 数 × 隐藏维度 × 每个数的字节数
```

第 3 节的输出里已经验证过一次（12 个 token，6144 字节）；第 5 节的表里，提示词 768 个加上生成的 32 个共 800 个 token：2 × 4 × 800 × 128 × 4 = 3276800 字节，和程序统计的一致。

这个公式里没有“参数个数”：KV Cache 的大小和模型有多少参数没有直接关系，它和**上下文长度成正比**，而且**每个请求各有一份**。

用一组假设的数字感受一下量级（注意是假设，真实的值看模型的 `config.json`）：32 层、隐藏维度 4096、每个数 2 字节、上下文 4096 个 token：

```
2 × 32 × 4096 × 4096 × 2 字节 = 2 147 483 648 字节 ≈ 2 GB      ← 一个请求
```

同时服务 20 个这样的请求就是 40 GB，可能比模型权重本身还大。所以在线上，**显存里能放多少个请求的 KV Cache，直接决定了能同时服务多少人**。由此引出三件事：

- **K、V 的头数可以比 Q 少**（上一课表里的 GQA）。K、V 每行的宽度变小，缓存按比例变小。真实模型的公式里“隐藏维度”那一项要换成“K、V 的头数 × 每个头的维度”。
- **缓存的内存怎么分配。** 请求来的时候不知道它会生成多长。按最大长度预留会浪费大量内存，按需增长又会产生碎片和搬运。[第 12 课](../lesson12/README.md) 的 PagedAttention 解决这个问题。
- **缓存可以复用。** 两个请求如果有相同的开头（比如相同的系统提示词），因果性保证了这段开头的 K、V 完全相同，可以只算一次、共用一份。这叫前缀缓存，也在第 12 课。

示例里的 `truncate` 是最简单的一种缓存管理：丢掉末尾的若干个位置。下一课的投机解码用它来撤销猜错的 token。

## 8. 和项目、岗位的关系

- **岗位**：“深入理解大模型的推理负载特性，熟悉 KV Cache 管理机制”“负责 Prefill 与 Decode 阶段的全链路性能分析、瓶颈定位”，对应的就是第 4～7 节。能说清楚“TTFT 由什么决定、TPOT 由什么决定、KV Cache 多大、为什么 Decode 受内存带宽限制”，这一部分就过关了。
- **项目**：平台上如果增加一个用小型语言模型生成缺陷说明的算子，它在产线上的耗时就要按 TTFT 和 TPOT 分开评估：提示词（包括图像变成的 token）决定 TTFT，说明文字的长度乘以 TPOT 决定剩下的时间。节拍固定时，要限制的是这两个量。
- **和 C++ 教程的关系**：第 16 课的推理服务把多个工位的请求攒成一批。那里一个请求算一次就结束；这里一个请求要算很多步，而且带着一份越来越大的状态（KV Cache）。第 11 课会把那个推理服务改造成适合这种负载的样子。

## 9. 动手运行

用 CMake（在 `docs/tutorial/inference` 目录下）：

```bash
cmake --build --preset default --target l09_equivalence l09_prefill_decode
```

```bash
./build/bin/l09_equivalence
```

```bash
./build/bin/l09_prefill_decode
```

或者直接调用编译器（在本文件夹下）。**macOS / Linux**：

```bash
clang++ -std=c++17 -I../common equivalence.cpp -o equivalence && ./equivalence
```

```bash
clang++ -std=c++17 -O2 -I../common prefill_decode.cpp -o prefill_decode && ./prefill_decode
```

**Windows**：

```bat
cl /std:c++17 /EHsc /utf-8 /I..\common equivalence.cpp && equivalence.exe
```

```bat
cl /std:c++17 /O2 /EHsc /utf-8 /I..\common prefill_decode.cpp && prefill_decode.exe
```

## 10. 练习

1. **故意写错一处**：把 `forwardCached` 里的 `embed(m, newTokens, startPos)` 改成 `embed(m, newTokens, 0)`（位置不接着数），运行 `equivalence`。哪一项检查先报错？误差有多大？这说明第 3 节那种检查的价值。做完改回去。
2. **只存 K 不存 V 行不行？** 不改代码，先回答：如果只缓存 K，Decode 时 V 从哪里来？要付出什么代价？再回答：为什么不需要缓存 Q？
3. **让 Prefill 快起来**：示例的 `linear` 对每一行分别遍历整个权重矩阵。用 [第 5 课](../lesson05/README.md) 学到的方法（至少：调整循环顺序和分块，让一小块权重连续用在多行上；有余力再加 SIMD 或多线程）重写它，重新运行 `prefill_decode`。“Prefill 每 token”和“TPOT”两列的比值变成了多少？为什么 Decode 得到的好处比 Prefill 少？
4. **估算一个真实模型**：用上一课练习 4 抄下来的 `config.json`，算一算：上下文 8192 个 token、每个数 2 字节时，一个请求的 KV Cache 占多少？（如果有 `num_key_value_heads`，记得用它。）一块 24 GB 显存的显卡放下模型权重之后，还能同时放几个这样的请求？
5. **滑动窗口**（选做）：上下文超过 `maxSeq` 时，一种简单的做法是丢掉最前面的若干个 token。给 `KvCache` 加一个“丢掉最前面 k 个位置”的函数。然后想一想：丢掉之后，剩下那些位置缓存的 K、V 还是正确的吗？（提示：它们算的时候看到了被丢掉的 token；位置编号也变了。）

## 11. 自测题

1. 不带缓存时，连续两步生成之间有哪些计算是重复的？为什么可以确定它们的结果相同？
2. 为什么缓存 K 和 V 就够了，不需要缓存 Q 和其他中间结果？
3. Prefill 和 Decode 各自的输入是什么？各做几次前向计算？
4. TTFT 和 TPOT 分别是什么？一个提示词很长、回答很短的请求，时间主要花在哪个阶段？
5. 为什么说 Prefill 比的是算力，Decode 比的是内存带宽？
6. 为什么我们的示例里看不出 Prefill 和 Decode 每个 token 耗时的差别？
7. KV Cache 的大小和哪些量成正比？和模型的参数个数有关吗？
8. 上下文变长时，Decode 一步的耗时为什么会慢慢变大？
9. 为什么说 KV Cache 的内存决定了一台机器能同时服务多少个请求？

## 下一课

[第 10 课：采样与 Speculative Decoding](../lesson10/README.md)
