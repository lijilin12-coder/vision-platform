// 第 10 课示例一：温度、top-k、top-p 对概率分布做了什么。
//
//   macOS / Linux：clang++ -std=c++17 -I../common sampling.cpp -o sampling && ./sampling
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common sampling.cpp && sampling.exe
#include <cstdio>
#include <vector>

#include "sampling.h"

using namespace sampling;

namespace {

void printProbs(const char* name, const std::vector<float>& probs) {
    std::printf("  %-22s", name);
    for (float p : probs) std::printf(" %5.3f", static_cast<double>(p));
    std::printf("\n");
}

int countNonZero(const std::vector<float>& probs) {
    int n = 0;
    for (float p : probs) n += p > 0 ? 1 : 0;
    return n;
}

double sumOf(const std::vector<float>& probs) {
    double s = 0;
    for (float p : probs) s += static_cast<double>(p);
    return s;
}

}  // namespace

int main() {
    // 假设词表只有 8 种 token，这是模型给出的打分
    const std::vector<float> logits = {3.0f, 2.5f, 2.0f, 1.0f, 0.5f, 0.0f, -1.0f, -2.0f};
    const int n = static_cast<int>(logits.size());
    bool ok = true;

    std::printf("token 编号               ");
    for (int i = 0; i < n; ++i) std::printf(" %5d", i);
    std::printf("\n  %-22s", "logits");
    for (float x : logits) std::printf(" %5.1f", static_cast<double>(x));
    std::printf("\n\n温度：\n");

    const std::vector<float> cold = toProbs(logits.data(), n, 0.5f);
    const std::vector<float> base = toProbs(logits.data(), n, 1.0f);
    const std::vector<float> hot = toProbs(logits.data(), n, 2.0f);
    printProbs("T = 0.5（更尖）", cold);
    printProbs("T = 1.0", base);
    printProbs("T = 2.0（更平）", hot);
    ok = ok && cold[0] > base[0] && base[0] > hot[0];

    std::printf("\n截断（都在 T = 1.0 的基础上）：\n");
    std::vector<float> k3 = base;
    topK(k3, 3);
    printProbs("top-k，k = 3", k3);
    ok = ok && countNonZero(k3) == 3;

    std::vector<float> p90 = base;
    topP(p90, 0.9f);
    printProbs("top-p，p = 0.9", p90);
    std::printf("    top-p 保留了 %d 个\n", countNonZero(p90));

    // 模型很确定时，top-p 保留得很少
    const std::vector<float> confident = {8.0f, 2.5f, 2.0f, 1.0f, 0.5f, 0.0f, -1.0f, -2.0f};
    std::vector<float> confidentP = toProbs(confident.data(), n, 1.0f);
    topP(confidentP, 0.9f);
    std::printf("    把第 0 个 token 的打分改成 8.0（模型很确定），top-p 只保留 %d 个\n", countNonZero(confidentP));
    ok = ok && countNonZero(confidentP) < countNonZero(p90);

    const std::vector<const std::vector<float>*> all = {&cold, &base, &hot, &k3, &p90};
    for (const std::vector<float>* probs : all) ok = ok && std::fabs(sumOf(*probs) - 1.0) < 1e-4;

    // 按概率抽 10 万次，各个 token 被抽到的比例应该接近它的概率
    std::printf("\n按 top-k 之后的概率抽 100000 次，各 token 被抽中的比例：\n");
    inftut::Rng rng(1);
    std::vector<int> counts(static_cast<std::size_t>(n), 0);
    const int draws = 100000;
    for (int i = 0; i < draws; ++i) ++counts[static_cast<std::size_t>(sampleFrom(k3, rng))];
    std::vector<float> freq;
    for (int c : counts) freq.push_back(static_cast<float>(c) / static_cast<float>(draws));
    printProbs("抽中的比例", freq);
    ok = ok && inftut::maxAbsDiff(freq, k3) < 0.01;

    // 贪心相当于温度趋近于 0：概率全部集中到打分最高的那个
    const std::vector<float> nearlyGreedy = toProbs(logits.data(), n, 0.01f);
    std::printf("\nT = 0.01 时第 0 个 token 的概率：%.6f（温度趋近 0 就是贪心）\n", static_cast<double>(nearlyGreedy[0]));
    ok = ok && nearlyGreedy[0] > 0.999f;

    if (!ok) {
        std::printf("错误：结果和预期不符\n");
        return 1;
    }
    return 0;
}
