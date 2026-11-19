// 第 10 课示例三：投机解码到底快不快？测量接受率、目标模型的前向次数和实际耗时。
//
// 一定要用优化模式编译：
//   macOS / Linux：clang++ -std=c++17 -O2 -I../common speculative_bench.cpp -o speculative_bench && ./speculative_bench
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 /I..\common speculative_bench.cpp && speculative_bench.exe
#include <cstdio>
#include <vector>

#include "speculative.h"

using namespace tinygpt;

int main() {
    Config cfg;
    cfg.vocab = 1000;
    cfg.dim = 96;
    cfg.heads = 4;
    cfg.layers = 8;      // 目标模型 8 层
    cfg.maxSeq = 128;
    const int draftLayers = 2;   // 草稿模型用前 2 层
    const int newTokens = 64;

    // 两段不同的提示词，结果合在一起统计
    std::vector<std::vector<int>> prompts;
    inftut::Rng rng(7);
    for (int p = 0; p < 2; ++p) {
        std::vector<int> prompt;
        for (int i = 0; i < 16; ++i) prompt.push_back(static_cast<int>(rng.below(static_cast<std::uint32_t>(cfg.vocab))));
        prompts.push_back(prompt);
    }
    const int totalTokens = newTokens * static_cast<int>(prompts.size());

    std::printf("目标模型 %d 层，草稿模型 %d 层，隐藏维度 %d；%zu 段提示词各生成 %d 个 token\n\n", cfg.layers, draftLayers, cfg.dim,
                prompts.size(), newTokens);

    // 先测草稿模型和目标模型各自 Decode 一步要多久
    double costRatio = 0;
    {
        const Model model = makeModel(cfg, 42);
        KvCache a(cfg);
        KvCache b(cfg);
        forwardCached(model, a, prompts[0]);
        forwardCached(model, b, prompts[0], draftLayers);
        const double targetMs = inftut::medianMs(15, [&] {
            const Mat logits = forwardCached(model, a, {1});
            inftut::keep(logits.v[0]);
            a.truncate(16);
        });
        const double draftMs = inftut::medianMs(15, [&] {
            const Mat logits = forwardCached(model, b, {1}, draftLayers);
            inftut::keep(logits.v[0]);
            b.truncate(16);
        });
        costRatio = draftMs / targetMs;
        std::printf("Decode 一步：目标模型 %.3f ms，草稿模型 %.3f ms，草稿是目标的 %.2f 倍\n\n", targetMs, draftMs, costRatio);
    }

    std::printf("%10s %4s %8s %14s %12s %12s %10s %16s\n", "lateScale", "k", "接受率", "每轮得到token", "只用目标 ms", "投机解码 ms",
                "实测倍数", "估算倍数(见正文)");
    bool ok = true;
    for (float lateScale : {0.0f, 0.05f, 0.1f, 0.2f, 1.0f}) {
        Model model = makeModel(cfg, 42);
        scaleLateLayers(model, draftLayers, lateScale);

        std::vector<std::vector<int>> expected(prompts.size());
        const double baselineMs = inftut::onceMs([&] {
            for (std::size_t p = 0; p < prompts.size(); ++p) expected[p] = generateGreedyCached(model, prompts[p], newTokens);
        });

        for (int k : {2, 4, 8}) {
            SpeculativeStats stats;
            std::vector<std::vector<int>> got(prompts.size());
            const double specMs = inftut::onceMs([&] {
                for (std::size_t p = 0; p < prompts.size(); ++p) {
                    got[p] = generateSpeculative(model, draftLayers, prompts[p], newTokens, k, stats);
                }
            });
            ok = ok && got == expected;
            // 估算：假设目标模型“一次验证 k+1 个 token”和“Decode 一个 token”一样快（GPU 上接近这样，原因见正文）。
            // 那么只用目标模型要 totalTokens 步；投机解码要 rounds 步，再加上草稿模型的 draftSteps × costRatio 步。
            const double estimated = totalTokens / (stats.rounds + stats.draftSteps * costRatio);
            std::printf("%10.2f %4d %7.0f%% %14.2f %12.1f %12.1f %10.2f %16.2f\n", static_cast<double>(lateScale), k,
                        100.0 * stats.accepted / stats.drafted, static_cast<double>(stats.generated) / stats.rounds, baselineMs,
                        specMs, baselineMs / specMs, estimated);
        }
    }
    if (!ok) {
        std::printf("错误：投机解码的输出和只用目标模型不一致\n");
        return 1;
    }
    return 0;
}
