// 第 13 课示例一：在线 softmax。
// 要算 “按 softmax 权重加权的和”，不需要先把全部打分拿到手：可以一块一块地看，只记三个量。
//
//   macOS / Linux：clang++ -std=c++17 -I../common online_softmax.cpp -o online_softmax && ./online_softmax
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common online_softmax.cpp && online_softmax.exe
#include <cmath>
#include <cstdio>
#include <vector>

#include "inftut/common.h"

namespace {

// 标准做法：拿到全部打分之后，减最大值、取 exp、归一化，再加权求和。
// 需要一个和打分一样长的数组来放权重。
float weightedSumFull(const std::vector<float>& scores, const std::vector<float>& values) {
    float maxScore = scores[0];
    for (float s : scores) maxScore = std::max(maxScore, s);
    std::vector<float> weights(scores.size());
    float sum = 0;
    for (std::size_t j = 0; j < scores.size(); ++j) {
        weights[j] = std::exp(scores[j] - maxScore);
        sum += weights[j];
    }
    float result = 0;
    for (std::size_t j = 0; j < scores.size(); ++j) result += weights[j] / sum * values[j];
    return result;
}

// 在线做法：打分一块一块地来。只记住三个数：
//   m：到目前为止见过的最大打分
//   l：到目前为止 Σ exp(s_j - m)
//   o：到目前为止 Σ exp(s_j - m) · v_j
// 新来一块时，最大值可能变大。旧的 l 和 o 是按旧的 m 算的，乘上 exp(m旧 - m新) 就换算成了按新的 m 算的。
struct OnlineSoftmax {
    float m = -INFINITY;
    float l = 0;
    float o = 0;

    void addBlock(const float* scores, const float* values, int count) {
        float mNew = m;
        for (int j = 0; j < count; ++j) mNew = std::max(mNew, scores[j]);
        const float rescale = std::exp(m - mNew);   // 第一块时 m = -∞，rescale = 0，而 l、o 本来就是 0
        l *= rescale;
        o *= rescale;
        for (int j = 0; j < count; ++j) {
            const float w = std::exp(scores[j] - mNew);
            l += w;
            o += w * values[j];
        }
        m = mNew;
    }

    float result() const { return o / l; }
};

// 不减最大值的 softmax：打分一大就溢出
float weightedSumNaive(const std::vector<float>& scores, const std::vector<float>& values) {
    float sum = 0;
    float weighted = 0;
    for (std::size_t j = 0; j < scores.size(); ++j) {
        const float w = std::exp(scores[j]);
        sum += w;
        weighted += w * values[j];
    }
    return weighted / sum;
}

}  // namespace

int main() {
    bool ok = true;
    inftut::Rng rng(11);
    const int n = 1000;
    std::vector<float> scores;
    std::vector<float> values;
    for (int j = 0; j < n; ++j) {
        scores.push_back(rng.normal() * 3.0f);
        values.push_back(rng.normal());
    }

    const float full = weightedSumFull(scores, values);
    std::printf("%d 个打分，标准做法的结果：%.6f\n\n", n, static_cast<double>(full));
    std::printf("在线做法，每块的大小不同：\n");
    for (int block : {1, 7, 64, 1000}) {
        OnlineSoftmax online;
        for (int start = 0; start < n; start += block) {
            online.addBlock(scores.data() + start, values.data() + start, std::min(block, n - start));
        }
        const double diff = std::fabs(static_cast<double>(online.result()) - static_cast<double>(full));
        std::printf("  每块 %4d 个：%.6f，和标准做法相差 %.2e\n", block, static_cast<double>(online.result()), diff);
        ok = ok && diff < 1e-5;
    }

    // 最大值在最后才出现：前面的块都要被重新缩放
    {
        std::vector<float> s = {1.0f, 2.0f, 0.5f, 3.0f, 50.0f, 49.0f};
        std::vector<float> v = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
        OnlineSoftmax online;
        std::printf("\n最大的打分出现在最后一块（每块 2 个）：\n");
        for (int start = 0; start < 6; start += 2) {
            online.addBlock(s.data() + start, v.data() + start, 2);
            std::printf("  看完第 %d 块：m = %5.1f，l = %.4f，o = %.4f，当前结果 o/l = %.4f\n", start / 2, static_cast<double>(online.m),
                        static_cast<double>(online.l), static_cast<double>(online.o), static_cast<double>(online.result()));
        }
        const float expected = weightedSumFull(s, v);
        std::printf("  标准做法：%.4f\n", static_cast<double>(expected));
        ok = ok && std::fabs(online.result() - expected) < 1e-5f;
    }

    // 为什么要减最大值
    {
        std::vector<float> s = {100.0f, 99.0f, 98.0f};
        std::vector<float> v = {1.0f, 2.0f, 3.0f};
        const float naive = weightedSumNaive(s, v);
        const float safe = weightedSumFull(s, v);
        std::printf("\n打分是 100、99、98 时：不减最大值算出来%s，减最大值算出来是 %.4f\n",
                    std::isnan(naive) ? "不是一个数（NaN）" : "是一个数", static_cast<double>(safe));
        ok = ok && std::isnan(naive) && !std::isnan(safe);
    }

    if (!ok) {
        std::printf("错误：结果和预期不符\n");
        return 1;
    }
    return 0;
}
