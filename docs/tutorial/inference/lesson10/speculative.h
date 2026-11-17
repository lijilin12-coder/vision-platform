// 投机解码（speculative decoding）的贪心版本。
// 草稿模型（小、快）连续猜 k 个 token，目标模型（大、慢）一次前向计算检查这 k 个猜得对不对。
#pragma once

#include <vector>

#include "../lesson09/kv_cache.h"

namespace tinygpt {

struct SpeculativeStats {
    int rounds = 0;          // 进行了多少轮“猜 k 个 → 验证”。每一轮目标模型做一次前向计算
    int drafted = 0;         // 草稿模型一共猜了多少个 token
    int accepted = 0;        // 其中被目标模型接受的
    int draftSteps = 0;      // 草稿模型做了多少次单 token 的前向计算
    int generated = 0;       // 一共生成了多少个 token
};

// 怎么得到一个“又便宜又和目标模型差不多”的草稿模型？
// 真实系统里用的是同一系列里小得多的模型。我们的模型是随机权重，小模型和大模型毫无关系，
// 所以这里【人为构造】一对：草稿模型 = 目标模型的前 draftLayers 层，
// 并把目标模型后面那些层的输出乘上 lateScale。lateScale 越小，后面的层对结果的影响越小，
// 只算前几层就越接近完整的结果。lateScale = 0 时两者完全相同。
inline void scaleLateLayers(Model& m, int draftLayers, float lateScale) {
    for (int l = draftLayers; l < m.cfg.layers; ++l) {
        Layer& layer = m.layers[static_cast<std::size_t>(l)];
        for (float& w : layer.wo.v) w *= lateScale;
        for (float& w : layer.w2.v) w *= lateScale;
        for (float& b : layer.b2) b *= lateScale;
    }
}

// 贪心投机解码。返回值和 generateGreedyCached(m, prompt, newTokens) 完全相同，只是计算的方式不同。
//   draftLayers：草稿模型用前几层
//   k：          每一轮草稿模型猜几个 token
// 提示词至少要有 1 个 token；提示词长度 + newTokens + k 不能超过 cfg.maxSeq。
inline std::vector<int> generateSpeculative(const Model& m, int draftLayers, std::vector<int> tokens, int newTokens, int k,
                                            SpeculativeStats& stats) {
    KvCache targetCache(m.cfg);
    KvCache draftCache(m.cfg);
    const int vocab = m.cfg.vocab;

    // 约定：两个缓存里都放着“除了最后一个 token 以外”的全部 token；最后一个 token 记在 pending 里，还没喂给模型。
    int pending = tokens.back();
    if (tokens.size() > 1) {
        const std::vector<int> head(tokens.begin(), tokens.end() - 1);
        forwardCached(m, targetCache, head);
        forwardCached(m, draftCache, head, draftLayers);
    }

    int remaining = newTokens;
    while (remaining > 0) {
        ++stats.rounds;
        const int base = targetCache.length;   // 这一轮开始时，两个缓存的长度

        // 1. 草稿模型从 pending 出发，一个接一个地猜 k 个 token
        std::vector<int> draft;
        int last = pending;
        for (int i = 0; i < k; ++i) {
            const Mat logits = forwardCached(m, draftCache, {last}, draftLayers);
            last = argmax(logits.row(0), vocab);
            draft.push_back(last);
            ++stats.draftSteps;
        }
        stats.drafted += k;

        // 2. 目标模型把 [pending, 猜的第 1 个, ..., 猜的第 k 个] 一次算完，得到 k+1 行 logits。
        //    第 i 行是“目标模型自己认为第 i+1 个应该是什么”。
        std::vector<int> batch = {pending};
        batch.insert(batch.end(), draft.begin(), draft.end());
        const Mat logits = forwardCached(m, targetCache, batch);

        // 3. 从前往后比，接受到第一个不一致为止
        int acceptedNow = 0;
        while (acceptedNow < k && draft[static_cast<std::size_t>(acceptedNow)] == argmax(logits.row(acceptedNow), vocab)) {
            ++acceptedNow;
        }
        stats.accepted += acceptedNow;

        // 4. 接受的 token，加上目标模型自己给出的下一个 token（猜错时是纠正，全对时是白送的一个）
        std::vector<int> emitted(draft.begin(), draft.begin() + acceptedNow);
        emitted.push_back(argmax(logits.row(acceptedNow), vocab));
        if (static_cast<int>(emitted.size()) > remaining) emitted.resize(static_cast<std::size_t>(remaining));
        tokens.insert(tokens.end(), emitted.begin(), emitted.end());
        remaining -= static_cast<int>(emitted.size());
        stats.generated += static_cast<int>(emitted.size());
        if (remaining == 0) break;

        // 5. 恢复约定：缓存里只留下 pending 和被接受的 token，猜错的那些位置丢掉
        pending = emitted.back();
        const int keep = base + 1 + acceptedNow;
        targetCache.truncate(keep);
        if (draftCache.length > keep) {
            draftCache.truncate(keep);
        } else if (draftCache.length < keep) {
            // k 个全对时，草稿模型的缓存里还缺它自己猜的最后一个 token
            forwardCached(m, draftCache, {draft.back()}, draftLayers);
            ++stats.draftSteps;
        }
    }
    return tokens;
}

}  // namespace tinygpt
