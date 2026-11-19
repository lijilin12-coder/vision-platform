// KV Cache：把每一层里每个位置算过的 K、V 向量存下来，后面的 token 直接用，不再重算。
// 在第 8 课的 tiny_gpt.h 上加了两样东西：KvCache 和带缓存的前向计算 forwardCached()。
#pragma once

#include <cstddef>
#include <vector>

#include "../lesson08/tiny_gpt.h"

namespace tinygpt {

// 一次生成（一个请求）对应一个 KvCache。
// 每一层有两块连续的内存：keys 和 values，各存 length 行，每行 dim 个 float。
struct KvCache {
    struct PerLayer {
        std::vector<float> keys;     // length × dim
        std::vector<float> values;   // length × dim
    };
    std::vector<PerLayer> layers;
    int dim = 0;
    int length = 0;   // 已经缓存了多少个位置

    KvCache() = default;
    explicit KvCache(const Config& cfg) : layers(static_cast<std::size_t>(cfg.layers)), dim(cfg.dim) {
        // 一次按最大长度留够空间，后面追加时不再重新申请内存（C++ 教程第 14 课）
        for (PerLayer& l : layers) {
            l.keys.reserve(static_cast<std::size_t>(cfg.maxSeq) * static_cast<std::size_t>(cfg.dim));
            l.values.reserve(static_cast<std::size_t>(cfg.maxSeq) * static_cast<std::size_t>(cfg.dim));
        }
    }

    // 缓存里实际存了多少字节的数据
    std::size_t bytes() const {
        std::size_t total = 0;
        for (const PerLayer& l : layers) total += (l.keys.size() + l.values.size()) * sizeof(float);
        return total;
    }

    // 只保留前 newLength 个位置，丢掉后面的（第 10 课的投机解码用它来“撤销”猜错的 token）
    void truncate(int newLength) {
        if (newLength >= length) return;
        for (PerLayer& l : layers) {
            const std::size_t keep = static_cast<std::size_t>(newLength) * static_cast<std::size_t>(dim);
            if (l.keys.size() > keep) l.keys.resize(keep);
            if (l.values.size() > keep) l.values.resize(keep);
        }
        length = newLength;
    }
};

// 带缓存的自注意力：x 只包含【新来的】m 个 token（m × dim），它们的位置是 startPos..startPos+m-1。
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

// 带缓存的前向计算：只处理新来的 token，返回它们的 logits（m × vocab），并把缓存加长 m 个位置。
//   Prefill：cache 是空的，newTokens 是整个提示词。
//   Decode： newTokens 只有一个 token（上一步刚生成的那个）。
// numLayers 是只算前几层（第 10 课用到）；-1 表示全部。
inline Mat forwardCached(const Model& m, KvCache& cache, const std::vector<int>& newTokens, int numLayers = -1) {
    const int layers = numLayers < 0 ? m.cfg.layers : numLayers;
    const int startPos = cache.length;
    Mat x = embed(m, newTokens, startPos);
    for (int l = 0; l < layers; ++l) {
        const Layer& layer = m.layers[static_cast<std::size_t>(l)];
        addInPlace(x, selfAttentionCached(layer, m.cfg, layerNorm(x, layer.norm1), cache.layers[static_cast<std::size_t>(l)], startPos));
        addInPlace(x, mlp(layer, layerNorm(x, layer.norm2)));
    }
    cache.length += static_cast<int>(newTokens.size());
    return toLogits(m, x);
}

// 带缓存的贪心生成：先 Prefill，再一步一步 Decode。
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

}  // namespace tinygpt
