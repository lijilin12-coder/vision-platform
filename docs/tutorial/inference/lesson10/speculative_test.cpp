// 第 10 课示例二：验证投机解码的两个性质。
//   1. 贪心版本：不管草稿模型猜得多差，最后的输出和只用目标模型完全一样。
//   2. 采样版本的接受/拒绝规则：按它抽出来的 token，分布和直接从目标模型的分布里抽是一样的。
//
//   macOS / Linux：clang++ -std=c++17 -I../common speculative_test.cpp -o speculative_test && ./speculative_test
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common speculative_test.cpp && speculative_test.exe
#include <cstdio>
#include <vector>

#include "sampling.h"
#include "speculative.h"

using namespace tinygpt;

namespace {

// 采样版本的规则。p 是目标模型的分布，q 是草稿模型的分布。
//   1. 草稿模型按 q 抽一个 token x。
//   2. 以 min(1, p[x] / q[x]) 的概率接受 x。
//   3. 不接受时，按 max(0, p - q) 归一化之后的分布重新抽一个。
int speculativeSample(const std::vector<float>& p, const std::vector<float>& q, inftut::Rng& rng, bool& accepted) {
    const int x = sampling::sampleFrom(q, rng);
    const float ratio = p[static_cast<std::size_t>(x)] / q[static_cast<std::size_t>(x)];
    accepted = rng.uniform() < ratio;
    if (accepted) return x;
    std::vector<float> residual(p.size());
    for (std::size_t i = 0; i < p.size(); ++i) residual[i] = std::max(0.0f, p[i] - q[i]);
    sampling::renormalize(residual);
    return sampling::sampleFrom(residual, rng);
}

}  // namespace

int main() {
    bool ok = true;

    // ---- 1. 贪心版本：输出必须和只用目标模型一样 ----
    Config cfg;
    cfg.layers = 4;       // 目标模型 4 层，草稿模型用前 1 层
    cfg.maxSeq = 96;
    const int draftLayers = 1;
    const std::vector<int> prompt = {5, 17, 3, 42, 8, 21};
    const int newTokens = 40;

    std::printf("贪心投机解码：目标模型 %d 层，草稿模型 %d 层，生成 %d 个 token\n", cfg.layers, draftLayers, newTokens);
    std::printf("%10s %4s %10s %10s %8s\n", "lateScale", "k", "接受率", "验证轮数", "输出");
    for (float lateScale : {0.0f, 0.1f, 0.3f, 1.0f}) {
        Model model = makeModel(cfg, 42);
        scaleLateLayers(model, draftLayers, lateScale);
        const std::vector<int> expected = generateGreedyCached(model, prompt, newTokens);
        for (int k : {1, 3, 8}) {
            SpeculativeStats stats;
            const std::vector<int> got = generateSpeculative(model, draftLayers, prompt, newTokens, k, stats);
            const bool same = got == expected;
            std::printf("%10.1f %4d %9.0f%% %10d %8s\n", static_cast<double>(lateScale), k,
                        100.0 * stats.accepted / stats.drafted, stats.rounds, same ? "相同" : "不同！");
            ok = ok && same && stats.generated == newTokens;
            // lateScale = 0 时草稿模型和目标模型完全一样，应该全部接受
            if (lateScale == 0.0f) ok = ok && stats.accepted == stats.drafted;
        }
    }

    // ---- 2. 采样版本的规则：抽出来的分布等于目标分布 p ----
    const std::vector<float> p = {0.50f, 0.20f, 0.15f, 0.10f, 0.05f};   // 目标模型
    const std::vector<float> q = {0.20f, 0.40f, 0.10f, 0.10f, 0.20f};   // 草稿模型：和 p 差得不少
    inftut::Rng rng(3);
    const int draws = 200000;
    std::vector<int> counts(p.size(), 0);
    int acceptedCount = 0;
    for (int i = 0; i < draws; ++i) {
        bool accepted = false;
        ++counts[static_cast<std::size_t>(speculativeSample(p, q, rng, accepted))];
        acceptedCount += accepted ? 1 : 0;
    }
    std::vector<float> freq;
    for (int c : counts) freq.push_back(static_cast<float>(c) / static_cast<float>(draws));
    // 理论上的接受率 = Σ min(p, q)
    float overlap = 0;
    for (std::size_t i = 0; i < p.size(); ++i) overlap += std::min(p[i], q[i]);

    std::printf("\n采样版本的接受/拒绝规则，抽 %d 次：\n", draws);
    std::printf("  目标分布 p    ：");
    for (float x : p) std::printf(" %.3f", static_cast<double>(x));
    std::printf("\n  草稿分布 q    ：");
    for (float x : q) std::printf(" %.3f", static_cast<double>(x));
    std::printf("\n  实际抽到的比例：");
    for (float x : freq) std::printf(" %.3f", static_cast<double>(x));
    std::printf("\n  接受率：实测 %.3f，理论值 Σ min(p, q) = %.3f\n", static_cast<double>(acceptedCount) / draws,
                static_cast<double>(overlap));
    ok = ok && inftut::maxAbsDiff(freq, p) < 0.01;
    ok = ok && std::fabs(static_cast<double>(acceptedCount) / draws - static_cast<double>(overlap)) < 0.01;

    if (!ok) {
        std::printf("错误：结果和预期不符\n");
        return 1;
    }
    return 0;
}
