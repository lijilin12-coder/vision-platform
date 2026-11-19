// 一个最小的 GPT（只有解码器的 Transformer）前向计算。只有头文件，只用标准库。
// 第 8 课讲它的每一步；第 9、10 课在它上面加 KV Cache、采样和投机解码。
//
// 权重是用固定种子随机生成的：这个模型没有训练过，生成的内容没有意义。
// 但它的计算过程、每一步的形状、耗时随长度的变化，和真实的大模型是一样的。
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "inftut/common.h"

namespace tinygpt {

// ---------- 矩阵：rows 行 cols 列，按行存放 ----------

struct Mat {
    int rows = 0;
    int cols = 0;
    std::vector<float> v;

    Mat() = default;
    Mat(int r, int c) : rows(r), cols(c), v(static_cast<std::size_t>(r) * static_cast<std::size_t>(c), 0.0f) {}

    float* row(int r) { return v.data() + static_cast<std::size_t>(r) * static_cast<std::size_t>(cols); }
    const float* row(int r) const { return v.data() + static_cast<std::size_t>(r) * static_cast<std::size_t>(cols); }
};

// ---------- 模型的规模和权重 ----------

struct Config {
    int vocab = 64;    // 词表大小：一共有多少种 token
    int dim = 32;      // 隐藏维度：每个 token 用多少个数表示
    int heads = 4;     // 注意力头数。dim 必须能被它整除
    int layers = 2;    // 层数
    int maxSeq = 64;   // 最多能处理多少个 token（位置向量表的行数）
};

// 归一化层的参数：每一维一个缩放系数、一个偏移
struct NormParams {
    std::vector<float> gain;
    std::vector<float> bias;
};

// 一层 = 一个注意力块 + 一个 MLP 块
struct Layer {
    NormParams norm1;
    Mat wq, wk, wv, wo;        // 注意力的四个矩阵，都是 dim × dim
    NormParams norm2;
    Mat w1;                    // MLP 第一层：4·dim × dim
    std::vector<float> b1;
    Mat w2;                    // MLP 第二层：dim × 4·dim
    std::vector<float> b2;
};

struct Model {
    Config cfg;
    Mat tokenEmbedding;        // vocab × dim：每种 token 一行
    Mat positionEmbedding;     // maxSeq × dim：每个位置一行
    std::vector<Layer> layers;
    NormParams normFinal;
    Mat head;                  // vocab × dim：把最后的向量变成对每种 token 的打分
};

inline Mat randomMat(int rows, int cols, float scale, inftut::Rng& rng) {
    Mat m(rows, cols);
    for (float& x : m.v) x = rng.normal() * scale;
    return m;
}

inline NormParams identityNorm(int dim) { return {std::vector<float>(static_cast<std::size_t>(dim), 1.0f), std::vector<float>(static_cast<std::size_t>(dim), 0.0f)}; }

// 用固定的种子生成一个模型。同样的 cfg 和 seed 在所有平台上得到完全相同的权重。
inline Model makeModel(const Config& cfg, std::uint64_t seed) {
    inftut::Rng rng(seed);
    Model m;
    m.cfg = cfg;
    const float s = 1.0f / std::sqrt(static_cast<float>(cfg.dim));
    m.tokenEmbedding = randomMat(cfg.vocab, cfg.dim, 1.0f, rng);
    m.positionEmbedding = randomMat(cfg.maxSeq, cfg.dim, 0.5f, rng);
    for (int l = 0; l < cfg.layers; ++l) {
        Layer layer;
        layer.norm1 = identityNorm(cfg.dim);
        layer.wq = randomMat(cfg.dim, cfg.dim, 2.0f * s, rng);
        layer.wk = randomMat(cfg.dim, cfg.dim, 2.0f * s, rng);
        layer.wv = randomMat(cfg.dim, cfg.dim, s, rng);
        layer.wo = randomMat(cfg.dim, cfg.dim, s, rng);
        layer.norm2 = identityNorm(cfg.dim);
        layer.w1 = randomMat(4 * cfg.dim, cfg.dim, s, rng);
        layer.b1.assign(static_cast<std::size_t>(4 * cfg.dim), 0.0f);
        layer.w2 = randomMat(cfg.dim, 4 * cfg.dim, 0.5f * s, rng);
        layer.b2.assign(static_cast<std::size_t>(cfg.dim), 0.0f);
        m.layers.push_back(std::move(layer));
    }
    m.normFinal = identityNorm(cfg.dim);
    m.head = randomMat(cfg.vocab, cfg.dim, 3.0f * s, rng);
    return m;
}

// 模型一共有多少个参数（权重的个数）
inline std::size_t parameterCount(const Model& m) {
    std::size_t n = m.tokenEmbedding.v.size() + m.positionEmbedding.v.size() + m.head.v.size() +
                    m.normFinal.gain.size() + m.normFinal.bias.size();
    for (const Layer& l : m.layers) {
        n += l.wq.v.size() + l.wk.v.size() + l.wv.v.size() + l.wo.v.size() + l.w1.v.size() + l.w2.v.size();
        n += l.b1.size() + l.b2.size() + l.norm1.gain.size() + l.norm1.bias.size() + l.norm2.gain.size() + l.norm2.bias.size();
    }
    return n;
}

// ---------- 基本运算 ----------

inline float dot(const float* a, const float* b, int n) {
    float sum = 0;
    for (int i = 0; i < n; ++i) sum += a[i] * b[i];
    return sum;
}

// 线性层：y = x · Wᵀ + b。x 是 n × in，w 是 out × in，结果是 n × out。
// 每一行（每个 token）独立计算，互不影响。
inline Mat linear(const Mat& x, const Mat& w, const std::vector<float>* bias = nullptr) {
    Mat y(x.rows, w.rows);
    for (int r = 0; r < x.rows; ++r) {
        const float* in = x.row(r);
        float* out = y.row(r);
        for (int o = 0; o < w.rows; ++o) {
            out[o] = dot(in, w.row(o), w.cols) + (bias ? (*bias)[static_cast<std::size_t>(o)] : 0.0f);
        }
    }
    return y;
}

// LayerNorm：把每一行变成均值 0、方差 1，再乘缩放系数、加偏移。每一行独立计算。
inline Mat layerNorm(const Mat& x, const NormParams& p) {
    Mat y(x.rows, x.cols);
    for (int r = 0; r < x.rows; ++r) {
        const float* in = x.row(r);
        float* out = y.row(r);
        float mean = 0;
        for (int i = 0; i < x.cols; ++i) mean += in[i];
        mean /= static_cast<float>(x.cols);
        float var = 0;
        for (int i = 0; i < x.cols; ++i) var += (in[i] - mean) * (in[i] - mean);
        var /= static_cast<float>(x.cols);
        const float inv = 1.0f / std::sqrt(var + 1e-5f);
        for (int i = 0; i < x.cols; ++i) {
            out[i] = (in[i] - mean) * inv * p.gain[static_cast<std::size_t>(i)] + p.bias[static_cast<std::size_t>(i)];
        }
    }
    return y;
}

// GELU 激活函数（tanh 近似）。作用和 ReLU 类似，只是在 0 附近是平滑的。
inline float gelu(float x) {
    return 0.5f * x * (1.0f + std::tanh(0.7978845608f * (x + 0.044715f * x * x * x)));
}

// a += b
inline void addInPlace(Mat& a, const Mat& b) {
    for (std::size_t i = 0; i < a.v.size(); ++i) a.v[i] += b.v[i];
}

// 把一组打分变成概率：先都减去最大值（避免 exp 溢出），取 exp，再除以总和。
inline void softmaxInPlace(float* x, int n) {
    float maxValue = x[0];
    for (int i = 1; i < n; ++i) maxValue = std::max(maxValue, x[i]);
    float sum = 0;
    for (int i = 0; i < n; ++i) {
        x[i] = std::exp(x[i] - maxValue);
        sum += x[i];
    }
    for (int i = 0; i < n; ++i) x[i] /= sum;
}

// ---------- 模型的各个部分 ----------

// 第一步：token 编号 → 向量。第 i 个 token 的向量 = 它的 token 向量 + 它所在位置的位置向量。
// startPos 是这批 token 里第一个的位置（第 9 课用到；从头处理时是 0）。
inline Mat embed(const Model& m, const std::vector<int>& tokens, int startPos = 0) {
    Mat x(static_cast<int>(tokens.size()), m.cfg.dim);
    for (int i = 0; i < x.rows; ++i) {
        const float* te = m.tokenEmbedding.row(tokens[static_cast<std::size_t>(i)]);
        const float* pe = m.positionEmbedding.row(startPos + i);
        float* out = x.row(i);
        for (int d = 0; d < m.cfg.dim; ++d) out[d] = te[d] + pe[d];
    }
    return x;
}

// 注意力的核心：一个位置（它的查询向量是 q）去“看”前 count 个位置。
// keys、values 的第 j 行是第 j 个位置的 K、V 向量。结果写进 out（长度 dim）。
// 每个头各算各的：把 dim 维切成 heads 段，每段 headDim 维。
// scores 是临时空间，长度至少 count。
inline void attendOne(const float* q, const float* keys, const float* values, int count, int dim, int heads,
                      std::vector<float>& scores, float* out) {
    const int headDim = dim / heads;
    const float scale = 1.0f / std::sqrt(static_cast<float>(headDim));
    for (int h = 0; h < heads; ++h) {
        const int offset = h * headDim;
        // 1. 打分：q 和每个位置的 k 做点积
        for (int j = 0; j < count; ++j) {
            scores[static_cast<std::size_t>(j)] =
                dot(q + offset, keys + static_cast<std::size_t>(j) * static_cast<std::size_t>(dim) + offset, headDim) * scale;
        }
        // 2. 打分变成权重（加起来等于 1）
        softmaxInPlace(scores.data(), count);
        // 3. 按权重把各个位置的 v 加起来
        for (int d = 0; d < headDim; ++d) out[offset + d] = 0;
        for (int j = 0; j < count; ++j) {
            const float w = scores[static_cast<std::size_t>(j)];
            const float* v = values + static_cast<std::size_t>(j) * static_cast<std::size_t>(dim) + offset;
            for (int d = 0; d < headDim; ++d) out[offset + d] += w * v[d];
        }
    }
}

// 自注意力块（不带缓存）：x 是 n × dim，结果也是 n × dim。
// 因果掩码体现在 attendOne 的 count 参数上：第 t 个位置只看第 0..t 个位置。
inline Mat selfAttention(const Layer& layer, const Config& cfg, const Mat& x) {
    const Mat q = linear(x, layer.wq);
    const Mat k = linear(x, layer.wk);
    const Mat v = linear(x, layer.wv);
    Mat mixed(x.rows, cfg.dim);
    std::vector<float> scores(static_cast<std::size_t>(x.rows));
    for (int t = 0; t < x.rows; ++t) {
        attendOne(q.row(t), k.v.data(), v.v.data(), t + 1, cfg.dim, cfg.heads, scores, mixed.row(t));
    }
    return linear(mixed, layer.wo);
}

// MLP 块：放大到 4 倍宽 → 激活 → 缩回来。每个位置独立计算。
inline Mat mlp(const Layer& layer, const Mat& x) {
    Mat hidden = linear(x, layer.w1, &layer.b1);
    for (float& value : hidden.v) value = gelu(value);
    return linear(hidden, layer.w2, &layer.b2);
}

// 最后一步：归一化，再用 head 矩阵算出对每种 token 的打分（logits）。结果是 n × vocab。
inline Mat toLogits(const Model& m, const Mat& x) { return linear(layerNorm(x, m.normFinal), m.head); }

// 完整的前向计算（不带缓存）：n 个 token 进去，n × vocab 的 logits 出来。
// 第 i 行是“看完第 0..i 个 token 之后，对第 i+1 个 token 的打分”。
// numLayers 是只算前几层（第 10 课用到）；-1 表示全部。
inline Mat forward(const Model& m, const std::vector<int>& tokens, int numLayers = -1) {
    const int layers = numLayers < 0 ? m.cfg.layers : numLayers;
    Mat x = embed(m, tokens);
    for (int l = 0; l < layers; ++l) {
        const Layer& layer = m.layers[static_cast<std::size_t>(l)];
        addInPlace(x, selfAttention(layer, m.cfg, layerNorm(x, layer.norm1)));   // 残差连接：x = x + 注意力(x)
        addInPlace(x, mlp(layer, layerNorm(x, layer.norm2)));                    // 残差连接：x = x + MLP(x)
    }
    return toLogits(m, x);
}

// 一行里最大的那个数的下标
inline int argmax(const float* x, int n) {
    int best = 0;
    for (int i = 1; i < n; ++i) {
        if (x[i] > x[best]) best = i;
    }
    return best;
}

// 贪心生成（不带缓存）：每一步把到目前为止的全部 token 重新算一遍，取打分最高的 token 接在后面。
inline std::vector<int> generateGreedy(const Model& m, std::vector<int> tokens, int newTokens) {
    for (int i = 0; i < newTokens; ++i) {
        const Mat logits = forward(m, tokens);
        tokens.push_back(argmax(logits.row(logits.rows - 1), m.cfg.vocab));
    }
    return tokens;
}

}  // namespace tinygpt
