// 采样：模型给出的是对每种 token 的打分（logits），怎么从里面选出下一个 token。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

#include "inftut/common.h"

namespace sampling {

// logits → 概率，带温度。
// 温度 T：先把每个打分除以 T，再做 softmax。T < 1 让分布更尖（更确定），T > 1 让分布更平（更随机）。
inline std::vector<float> toProbs(const float* logits, int n, float temperature) {
    std::vector<float> probs(static_cast<std::size_t>(n));
    float maxValue = logits[0];
    for (int i = 1; i < n; ++i) maxValue = std::max(maxValue, logits[i]);
    float sum = 0;
    for (int i = 0; i < n; ++i) {
        probs[static_cast<std::size_t>(i)] = std::exp((logits[i] - maxValue) / temperature);
        sum += probs[static_cast<std::size_t>(i)];
    }
    for (float& p : probs) p /= sum;
    return probs;
}

// 让概率重新加起来等于 1
inline void renormalize(std::vector<float>& probs) {
    const float sum = std::accumulate(probs.begin(), probs.end(), 0.0f);
    for (float& p : probs) p /= sum;
}

// 返回按概率从大到小排好的下标
inline std::vector<int> sortedByProb(const std::vector<float>& probs) {
    std::vector<int> order(probs.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return probs[static_cast<std::size_t>(a)] > probs[static_cast<std::size_t>(b)]; });
    return order;
}

// top-k：只保留概率最大的 k 个，其余清零，再归一化。
inline void topK(std::vector<float>& probs, int k) {
    const std::vector<int> order = sortedByProb(probs);
    for (std::size_t i = static_cast<std::size_t>(k); i < order.size(); ++i) probs[static_cast<std::size_t>(order[i])] = 0;
    renormalize(probs);
}

// top-p（也叫 nucleus）：从大到小累加，加到总和刚达到 p 为止，保留这些，其余清零，再归一化。
// 和 top-k 的区别：保留几个不是固定的。模型很确定时只留一两个，拿不准时留很多个。
inline void topP(std::vector<float>& probs, float p) {
    const std::vector<int> order = sortedByProb(probs);
    float cumulative = 0;
    std::size_t keep = 0;
    while (keep < order.size() && cumulative < p) {
        cumulative += probs[static_cast<std::size_t>(order[keep])];
        ++keep;
    }
    for (std::size_t i = keep; i < order.size(); ++i) probs[static_cast<std::size_t>(order[i])] = 0;
    renormalize(probs);
}

// 按概率抽一个：在 [0,1) 上取一个随机数，看它落在哪一段。
inline int sampleFrom(const std::vector<float>& probs, inftut::Rng& rng) {
    const float r = rng.uniform();
    float cumulative = 0;
    int last = 0;
    for (std::size_t i = 0; i < probs.size(); ++i) {
        if (probs[i] <= 0) continue;
        cumulative += probs[i];
        last = static_cast<int>(i);
        if (r < cumulative) return last;
    }
    return last;  // 浮点误差导致累加和略小于 1 时，返回最后一个概率不为 0 的
}

}  // namespace sampling
