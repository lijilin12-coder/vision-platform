# 第 8 课：Transformer：手写一个最小的 GPT 前向

> 预计用时：2.5 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

前七课的对象是视觉模型：一张图进去，一个结果出来，算一次就结束。从这一课开始是第二部分，对象换成大语言模型。它的推理方式不一样：**一次只产生一个 token，产生几百个 token 就要算几百次**。后面几课的所有优化（KV Cache、Continuous Batching、PagedAttention、投机解码）都是围绕这一点展开的，所以要先把一次前向计算里到底做了什么弄清楚。

1. 大模型是怎么“生成文字”的？
2. 一次前向计算分哪几步，每一步的输入输出是什么形状？
3. 为什么不做优化的话，生成得越长就越慢？

这一课不讲训练，也不讲模型为什么有效，只讲推理时的计算过程。示例里的模型权重是随机数，生成的内容没有意义，但计算过程和真实的模型一样。

## 1. 大模型只做一件事：预测下一个 token

**token** 是模型处理文字的最小单位。一段文字先被切成一串 token（一个 token 大致是一个汉字、一个词或者半个英文单词），每种 token 有一个整数编号。全部编号的集合叫**词表**（vocabulary），大小通常是几万到十几万。把文字切成 token、再把 token 拼回文字的程序叫分词器（tokenizer），它和推理性能关系不大，这套教程不展开，示例里直接用整数编号。

模型的输入是一串 token 编号，输出是**对“下一个 token 是哪一种”的打分**：词表里的每一种 token 一个分数，分数越高表示越可能。这组分数叫 **logits**。

生成一段文字就是反复做这件事：

```
提示词：        [5, 17, 3]
第 1 步：模型看 [5, 17, 3]          → 打分最高的是 42 → 接在后面
第 2 步：模型看 [5, 17, 3, 42]      → 打分最高的是 8  → 接在后面
第 3 步：模型看 [5, 17, 3, 42, 8]   → 打分最高的是 21 → 接在后面
……直到生成了结束标记，或者达到长度上限
```

这种“把自己的输出接回输入，再算下一个”的方式叫**自回归**。每一步都取打分最高的 token，叫**贪心**生成（第 10 课讲别的取法）。

对比一下：第 1 课的卷积网络对一张图只做一次前向计算。这里输出 500 个 token 就要做 500 次前向计算，而且每一次的输入都比上一次长一个 token。

## 2. 模型的整体结构

GPT、Llama、Qwen、DeepSeek 这些模型都属于“只有解码器的 Transformer”（decoder-only），结构是同一种：

```
token 编号（n 个整数）
      │
      ▼
┌──────────────┐
│  Embedding   │   每个编号查表，变成一个 dim 维的向量           → n × dim
└──────────────┘
      │
      ▼
┌──────────────┐ ─┐
│  归一化       │  │
│  自注意力     │  │  每个位置去“看”它前面的位置，把信息汇总过来
│  （加回原值） │  │
│  归一化       │  │  一层。整个模型把这样的层重复 layers 次         → n × dim
│  MLP         │  │  每个位置自己做一次非线性变换
│  （加回原值） │  │
└──────────────┘ ─┘
      │
      ▼
┌──────────────┐
│ 归一化 + Head │   每个位置的向量变成对词表里每种 token 的打分    → n × vocab
└──────────────┘
```

模型的规模由几个数决定（[`tiny_gpt.h`](tiny_gpt.h)）：

```cpp
struct Config {
    int vocab = 64;    // 词表大小：一共有多少种 token
    int dim = 32;      // 隐藏维度：每个 token 用多少个数表示
    int heads = 4;     // 注意力头数。dim 必须能被它整除
    int layers = 2;    // 层数
    int maxSeq = 64;   // 最多能处理多少个 token
};
```

整个计算过程中，数据始终是一个 **n 行 dim 列**的矩阵：一行对应一个 token。这一点要记住，后面所有的优化都在这个矩阵上做文章。

## 3. 第一步：Embedding

token 编号只是一个整数，不能直接参与计算。模型里有一张表 `tokenEmbedding`，`vocab` 行 `dim` 列，第 k 行就是第 k 种 token 对应的向量。Embedding 就是查表。

只查这张表有一个问题：同一个 token 出现在第 1 个位置和第 10 个位置，查出来的向量一样，模型分不出先后。所以还有一张位置表 `positionEmbedding`，第 i 行是“第 i 个位置”的向量，两者相加：

```cpp
inline Mat embed(const Model& m, const std::vector<int>& tokens, int startPos = 0) {
    Mat x(static_cast<int>(tokens.size()), m.cfg.dim);
    for (int i = 0; i < x.rows; ++i) {
        const float* te = m.tokenEmbedding.row(tokens[i]);          // 这种 token 的向量
        const float* pe = m.positionEmbedding.row(startPos + i);    // 这个位置的向量
        float* out = x.row(i);
        for (int d = 0; d < m.cfg.dim; ++d) out[d] = te[d] + pe[d];
    }
    return x;
}
```

这一步没有乘法，只有查表和加法，耗时可以忽略。

## 4. 自注意力

到这里，每一行只包含它自己那个 token 的信息。但预测下一个 token 需要上文：读到“这个零件的表面有一道”时，要知道前面说的是“零件”和“表面”。**自注意力**（self-attention）是模型里唯一让不同位置之间交换信息的地方。

### 一个位置怎么“看”别的位置

每个位置的向量先分别乘三个矩阵，得到三个新向量：

| 向量 | 名字 | 作用 |
|---|---|---|
| q = x · Wq | 查询（query） | “我想找什么样的信息” |
| k = x · Wk | 键（key） | “我这里有什么样的信息” |
| v = x · Wv | 值（value） | “如果你要，我给出的内容是这个” |

然后第 t 个位置做三件事：

1. **打分**：用自己的 q 和每个位置 j 的 k 做点积，得到一个分数 s_j。点积越大，表示第 j 个位置越“对得上”。
2. **变成权重**：对这组分数做 softmax，得到一组加起来等于 1 的权重。
3. **加权求和**：按权重把各个位置的 v 加起来，就是第 t 个位置这一轮收集到的信息。

```cpp
// 1. 打分：q 和每个位置的 k 做点积
for (int j = 0; j < count; ++j) scores[j] = dot(q + offset, keys + j * dim + offset, headDim) * scale;
// 2. 打分变成权重（加起来等于 1）
softmaxInPlace(scores.data(), count);
// 3. 按权重把各个位置的 v 加起来
for (int j = 0; j < count; ++j) {
    const float w = scores[j];
    const float* v = values + j * dim + offset;
    for (int d = 0; d < headDim; ++d) out[offset + d] += w * v[d];
}
```

`scale` 是 1/√headDim，作用是不让点积随着维度变大而变大。最后的结果还要再乘一个矩阵 Wo。

### 多头

上面的代码里有 `offset` 和 `headDim`。实际的做法是把 dim 维切成 `heads` 段，每一段独立地做一遍“打分、softmax、加权求和”，叫一个**头**。dim = 32、4 个头时，每个头用 8 维。这样同一个位置可以同时用几种不同的方式去看上文。对推理来说，多头只是把同样的计算分成几份，总计算量不变。

### 因果掩码：只能看前面，不能看后面

生成的时候，第 t 个位置后面还没有 token，当然看不到。但提示词是一次性送进去的，第 2 个位置后面明明有第 3、4、5 个。这时也**不允许**往后看：模型训练时每个位置都只看到了它前面的内容，推理时要保持一致。

用矩阵表示，5 个 token 的打分是一个 5 × 5 的表，第 i 行是第 i 个位置对各个位置的打分。右上角的那一半要挡掉，做法是把它们设成负无穷，softmax 之后权重就是 0。这个“挡掉右上角”的操作叫**因果掩码**（causal mask）：

```
            被看的位置 j
            0    1    2    3    4
看的     0 [ s    -∞   -∞   -∞   -∞ ]
位置 i   1 [ s    s    -∞   -∞   -∞ ]
         2 [ s    s    s    -∞   -∞ ]
         3 [ s    s    s    s    -∞ ]
         4 [ s    s    s    s    s  ]
```

示例里没有真的建这张表，而是让第 t 个位置的循环只走到 t 为止，效果相同（第 13 课会回到“要不要建这张表”的问题）：

```cpp
for (int t = 0; t < x.rows; ++t) {
    attendOne(q.row(t), k.v.data(), v.v.data(), t + 1, ...);   // 只看前 t+1 个位置：第 0..t 个
}
```

因果掩码带来一个非常重要的性质：**第 i 个位置的计算结果只取决于第 0..i 个 token，后面再接什么都不会改变它。** 示例里做了验证（见第 7 节）。下一课的 KV Cache 完全建立在这个性质上。

## 5. MLP、残差和归一化

### MLP

注意力之后，每个位置各自经过一个两层的全连接网络：先放大到 4 倍宽，过一个激活函数，再缩回来。

```cpp
inline Mat mlp(const Layer& layer, const Mat& x) {
    Mat hidden = linear(x, layer.w1, &layer.b1);     // dim → 4·dim
    for (float& value : hidden.v) value = gelu(value);
    return linear(hidden, layer.w2, &layer.b2);      // 4·dim → dim
}
```

这一步里各个位置之间**没有任何联系**，每一行自己算自己的。`gelu` 是一个和 ReLU 作用类似的激活函数。

### 残差连接

注意力和 MLP 的结果都不是直接替换原来的 x，而是**加**到 x 上：

```cpp
addInPlace(x, selfAttention(layer, m.cfg, layerNorm(x, layer.norm1)));   // x = x + 注意力(归一化(x))
addInPlace(x, mlp(layer, layerNorm(x, layer.norm2)));                    // x = x + MLP(归一化(x))
```

这叫残差连接。可以把 x 理解成一条主线，每一层往上面添一点修正。它对训练很重要；对推理来说只是多一次加法。（第 10 课会利用“每层只是添一点修正”这个特点来构造一个草稿模型。）

### 归一化

每个子块之前先做一次归一化，把每一行的数值调整到稳定的范围。示例用的是 **LayerNorm**：每一行减去自己的均值、除以自己的标准差，再乘一组可学习的缩放系数、加一组偏移。它也是每一行独立计算。

### 真实模型里的几种变体

示例用的是最早的 GPT 的写法。Llama、Qwen 这一类较新的模型结构相同，但几个部件换了做法。读它们的代码时要认得出来：

| 部件 | 示例里的做法 | Llama、Qwen 这类模型常见的做法 | 区别 |
|---|---|---|---|
| 归一化 | LayerNorm | **RMSNorm** | 不减均值，只除以“平方的平均值再开方”，只有缩放系数、没有偏移。计算更少 |
| 位置信息 | 输入处加一个位置向量 | **RoPE**（旋转位置编码） | 输入处不加。改为在每一层的注意力里，按位置把 q 和 k 旋转一个角度，使打分取决于两个位置的距离 |
| MLP | 两个矩阵 + GELU | 三个矩阵 + 门控（SwiGLU） | 多一个矩阵，激活方式不同 |
| K、V 的头数 | 和 Q 一样多 | 可以更少（分组查询注意力，GQA） | 几个查询头共用一组 K、V，下一课的 KV Cache 因此变小 |

这些变体不改变这一课的结论：数据是 n × dim 的矩阵；只有注意力让不同位置交换信息；第 i 个位置只取决于第 0..i 个 token。

## 6. 最后一步：logits

所有层算完之后，再做一次归一化，然后乘一个 `vocab × dim` 的矩阵 `head`，每一行变成 `vocab` 个打分：

```cpp
inline Mat toLogits(const Model& m, const Mat& x) { return linear(layerNorm(x, m.normFinal), m.head); }
```

结果是 n × vocab。第 i 行是“看完第 0..i 个 token 之后，对第 i+1 个 token 的打分”。**生成时只用最后一行**：

```cpp
inline std::vector<int> generateGreedy(const Model& m, std::vector<int> tokens, int newTokens) {
    for (int i = 0; i < newTokens; ++i) {
        const Mat logits = forward(m, tokens);                                 // 全部 token 重新算一遍
        tokens.push_back(argmax(logits.row(logits.rows - 1), m.cfg.vocab));    // 只用最后一行
    }
    return tokens;
}
```

注意这里的浪费：算出了 n 行 logits，只用了 1 行；而且下一步又要把这 n 个 token 全部重新算一遍。

## 7. 运行示例

[`forward.cpp`](forward.cpp) 把上面的过程走一遍：

```
模型：词表 64，隐藏维度 32，4 个头，2 层，共 31360 个参数

输入 6 个 token：5 17 3 42 8 21

每一步的形状（行数 = token 数）：
  embedding 之后       ：6 × 32
  第 0 层之后          ：6 × 32
  第 1 层之后          ：6 × 32
  logits              ：6 × 64

最后一行 logits 变成概率后，最大的是 token 57，概率 0.270434

把最后一个 token 从 21 改成 60：
  前 5 行 logits 的最大变化：0
  最后一行 logits 的最大变化：11.4779

贪心生成 12 个 token：5 17 3 42 8 21 57 17 39 18 54 25 37 59 13 54 32 40
```

看三处：

- **形状**：从头到尾都是 6 行，直到最后才从 32 列变成 64 列。
- **因果性**：只改最后一个 token，前 5 行的输出**一点都没变**（变化是 0），只有最后一行变了。
- **生成**：6 个 token 的提示词后面接了 12 个新 token。权重是随机的，所以这串数字没有含义。

## 8. 计算量：为什么越长越慢

先数一数处理**一个 token、一层**要做多少次乘法（dim 记作 d）：

| 部分 | 矩阵 | 乘法次数 |
|---|---|---|
| 注意力 | Wq、Wk、Wv、Wo，各 d × d | 4·d² |
| MLP | W1（4d × d）、W2（d × 4d） | 8·d² |
| 注意力里的打分和加权求和 | 和前面 n 个位置各做一次 | 约 2·n·d |

所以 n 个 token、L 层的一次前向计算，乘法次数大约是 **L ×（12·d²·n + d·n²）**。前一项来自矩阵乘法，和 token 数成正比；后一项来自注意力，和 token 数的平方成正比。d 比 n 大的时候（短文本），前一项占绝大部分。

[`cost_growth.cpp`](cost_growth.cpp) 用一个大一些的模型（隐藏维度 128、4 层）测量一次前向计算的耗时。数字来自一台 2.1 GHz 的机器，单线程，你的结果会不同，看比例：

```
模型：隐藏维度 128，4 层，4 个头，词表 1000，共 1.11 百万个参数

已有 n 个 token 时，生成下一个 token 的耗时（一次完整的前向计算）：
       n    耗时 ms  每个 token ms
      16         7.93           0.4956
      32        15.97           0.4990
      64        32.77           0.5121
     128        67.18           0.5249
     256       152.29           0.5949
     512       358.87           0.7009

估算：不带缓存，从头连续生成 512 个 token，总共约 83.7 秒
如果每个 token 只算一次（下一课的 KV Cache），512 个 token 约 0.36 秒
```

- token 数翻倍，耗时大约翻倍，后面略多于翻倍（n² 那一项开始显现）。“每个 token”那一列从 0.50 涨到 0.70 毫秒，涨的部分就是注意力。
- **生成第 n 个 token 要花和 n 成正比的时间**。那么连续生成 512 个 token，总时间和 512² 成正比：约 84 秒。
- 而这 512 次前向计算里，绝大部分是在重复计算前面已经算过的位置。如果每个 token 只算一次，总共只要 0.36 秒，相差两百多倍。

第 4 节已经说明了这些重复计算的结果是不变的。把它们存下来不再重算，就是下一课的 KV Cache。

## 9. 这份代码和真实模型的对应

真实模型的结构参数写在模型目录的 `config.json` 里。下面是 Hugging Face 上 Llama、Qwen 这类模型常用的字段名，和这份代码的对应关系：

| `config.json` 里的字段 | 这份代码里 | 含义 |
|---|---|---|
| `vocab_size` | `Config::vocab` | 词表大小 |
| `hidden_size` | `Config::dim` | 隐藏维度 |
| `num_hidden_layers` | `Config::layers` | 层数 |
| `num_attention_heads` | `Config::heads` | 注意力头数 |
| `intermediate_size` | 代码里固定为 `4 * dim` | MLP 中间那一层的宽度 |
| `max_position_embeddings` | `Config::maxSeq` | 最大长度 |
| `num_key_value_heads` | 代码里等于 `heads` | K、V 的头数（比 `num_attention_heads` 小就是用了 GQA） |

**具体的数值不要背，也不要相信别人转述的**，直接打开你要部署的那个模型的 `config.json` 看。练习 4 要求你做这件事。

有了这几个数，就可以用第 8 节的公式估算计算量，用 [`parameterCount`](tiny_gpt.h) 那样的方法估算参数个数。参数个数乘以每个参数的字节数（32 位浮点是 4，16 位是 2，[第 6 课](../lesson06/README.md) 的 8 位量化是 1）就是模型占的内存。

## 10. 和项目、岗位的关系

- **和前面几课的关系**：这一课的 `linear` 就是矩阵乘法，耗时的绝大部分都在这里。示例里用的是最直接的三重循环，[第 5 课](../lesson05/README.md) 的全部方法（循环顺序、分块、SIMD）都能直接用上。Transformer 在算子层面比卷积网络简单：主要就是矩阵乘法、softmax、归一化这几种。难点在算子之外：怎么调度“一次一个 token”的循环。
- **和项目的关系**：路线图里计划把异常检测的骨干网络换成 ViT 一类的模型。ViT 的每一层和这一课的一层几乎相同，区别是没有因果掩码（图像的各个块可以互相看），也没有自回归循环。读懂了这一课，[第 7 课](../lesson07/README.md) 里把 ViT 放到推理后端上时遇到的算子就都认识了。
- **和岗位的关系**：招聘要求里的“深入理解 Transformer 架构及经典大模型结构（Qwen/Llama/Deepseek）及算子”，要求的就是这一课的内容加上第 5 节那张变体表。面试时常见的问法是“一次前向计算里各部分的计算量是多少”“为什么 Decode 阶段慢”，答案在第 8 节。

## 11. 动手运行

用 CMake（在 `docs/tutorial/inference` 目录下）：

```bash
cmake --preset default
```

```bash
cmake --build --preset default --target l08_forward l08_cost_growth
```

```bash
./build/bin/l08_forward
```

```bash
./build/bin/l08_cost_growth
```

或者直接调用编译器（在本文件夹下）。**macOS / Linux**：

```bash
clang++ -std=c++17 -I../common forward.cpp -o forward && ./forward
```

```bash
clang++ -std=c++17 -O2 -I../common cost_growth.cpp -o cost_growth && ./cost_growth
```

**Windows**：

```bat
cl /std:c++17 /EHsc /utf-8 /I..\common forward.cpp && forward.exe
```

```bat
cl /std:c++17 /O2 /EHsc /utf-8 /I..\common cost_growth.cpp && cost_growth.exe
```

## 12. 练习

1. **去掉因果掩码**：把 `selfAttention` 里的 `t + 1` 改成 `x.rows`（每个位置都能看到全部位置），重新运行 `forward`。因果性的检查还能通过吗？前 5 行 logits 的最大变化是多少？做完改回去。
2. **数参数**：不看程序输出，用第 8 节的表手算示例模型（词表 64、dim 32、2 层、maxSeq 64）的参数个数，和程序打印的 31360 对一下。提示：别忘了两张 embedding 表、`head`、归一化的参数和 MLP 的偏移。
3. **各部分占多少时间**：在 `cost_growth.cpp` 里，分别给 `selfAttention` 里的三次 `linear`、`attendOne` 的循环、`mlp` 计时（可以用全局变量累加）。n = 16 和 n = 512 时，三者的占比各是多少？和第 8 节的公式一致吗？
4. **看一个真实模型的配置**：在 Hugging Face 或魔搭社区上找一个你感兴趣的开源模型（比如某个规模的 Qwen），打开它的 `config.json`，把第 9 节表里的字段抄下来。用第 8 节的公式估算：它处理一个 token 大约要做多少次乘法？是示例里那个“隐藏维度 128、4 层”的模型的多少倍？
5. **实现 RMSNorm**（选做）：照着第 5 节表里的描述写一个 `rmsNorm` 函数，替换掉 `layerNorm`。模型还能运行吗？生成的 token 变了吗？为什么变了也不能说明谁对谁错？

## 13. 自测题

1. 什么是 token？什么是 logits？logits 有多少个数？
2. “自回归”是什么意思？为什么输出 500 个 token 要做 500 次前向计算？
3. 一次前向计算里，数据的形状是怎么变化的？
4. 自注意力里的 Q、K、V 各起什么作用？一个位置的输出是怎么算出来的？
5. 模型里哪些步骤是“每个位置独立计算”的？哪一步让不同位置交换信息？
6. 因果掩码挡掉了什么？它带来了什么性质？这个性质怎么用实验验证？
7. 一个 token 经过一层，大约做多少次乘法？哪一项和 token 数的平方成正比？
8. 不做任何优化时，连续生成 n 个 token 的总时间和 n 是什么关系？浪费在哪里？
9. RMSNorm、RoPE 分别替换了示例里的哪个部件？

## 下一课

[第 9 课：KV Cache：Prefill 与 Decode](../lesson09/README.md)
