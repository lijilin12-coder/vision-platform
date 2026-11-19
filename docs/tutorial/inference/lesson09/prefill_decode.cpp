// 第 9 课示例二：把一次请求分成 Prefill 和 Decode 两个阶段，分别测量。
//   首字延时 TTFT（time to first token）：从收到请求到给出第一个 token
//   每 token 耗时 TPOT（time per output token）：之后每生成一个 token 要多久
//
// 一定要用优化模式编译：
//   macOS / Linux：clang++ -std=c++17 -O2 -I../common prefill_decode.cpp -o prefill_decode && ./prefill_decode
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 /I..\common prefill_decode.cpp && prefill_decode.exe
#include <cstdio>
#include <vector>

#include "kv_cache.h"

using namespace tinygpt;

namespace {

Config benchConfig() {
    Config cfg;
    cfg.vocab = 1000;
    cfg.dim = 128;
    cfg.heads = 4;
    cfg.layers = 4;
    cfg.maxSeq = 1024;
    return cfg;
}

std::vector<int> randomTokens(int n, int vocab, std::uint64_t seed) {
    inftut::Rng rng(seed);
    std::vector<int> tokens;
    for (int i = 0; i < n; ++i) tokens.push_back(static_cast<int>(rng.below(static_cast<std::uint32_t>(vocab))));
    return tokens;
}

}  // namespace

int main() {
    const Config cfg = benchConfig();
    const Model model = makeModel(cfg, 42);
    std::printf("模型：隐藏维度 %d，%d 层，%d 个头，词表 %d\n", cfg.dim, cfg.layers, cfg.heads, cfg.vocab);

    // ---- 一、带缓存和不带缓存：提示词 32 个 token，生成 48 个 token ----
    {
        const std::vector<int> prompt = randomTokens(32, cfg.vocab, 7);
        const int newTokens = 48;
        std::vector<int> a;
        std::vector<int> b;
        const double noCacheMs = inftut::onceMs([&] { a = generateGreedy(model, prompt, newTokens); });
        const double cacheMs = inftut::onceMs([&] { b = generateGreedyCached(model, prompt, newTokens); });
        std::printf("\n提示词 32 个 token，生成 48 个 token：\n");
        std::printf("  不带缓存：%8.1f ms\n", noCacheMs);
        std::printf("  带缓存：  %8.1f ms（快 %.0f 倍），生成的 token %s\n", cacheMs, noCacheMs / cacheMs, a == b ? "完全相同" : "不同！");
        if (a != b) return 1;
    }

    // ---- 二、不同长度的提示词：TTFT 和 TPOT ----
    std::printf("\n不同长度的提示词，各生成 32 个 token：\n");
    std::printf("%10s %12s %18s %12s %16s\n", "提示词长度", "TTFT ms", "Prefill 每token ms", "TPOT ms", "KV Cache 字节");
    for (int promptLength : {16, 64, 256, 768}) {
        const std::vector<int> prompt = randomTokens(promptLength, cfg.vocab, 7);
        KvCache cache(cfg);
        Mat logits;
        // Prefill：一次处理整个提示词。它结束时，第一个输出 token 就确定了，所以 Prefill 的耗时就是 TTFT
        const double ttft = inftut::onceMs([&] { logits = forwardCached(model, cache, prompt); });
        // Decode：每次一个 token
        const int newTokens = 32;
        std::vector<double> stepMs;
        int next = argmax(logits.row(logits.rows - 1), cfg.vocab);
        for (int i = 0; i < newTokens; ++i) {
            stepMs.push_back(inftut::onceMs([&] { logits = forwardCached(model, cache, {next}); }));
            next = argmax(logits.row(0), cfg.vocab);
        }
        std::sort(stepMs.begin(), stepMs.end());
        const double tpot = stepMs[stepMs.size() / 2];
        std::printf("%10d %12.2f %18.4f %12.4f %16zu\n", promptLength, ttft, ttft / promptLength, tpot, cache.bytes());

        const std::size_t expected = 2u * static_cast<std::size_t>(cfg.layers) * static_cast<std::size_t>(promptLength + newTokens) *
                                     static_cast<std::size_t>(cfg.dim) * sizeof(float);
        if (cache.bytes() != expected) {
            std::printf("错误：KV Cache 的大小和公式不符（公式：%zu）\n", expected);
            return 1;
        }
    }

    // ---- 三、Decode 一步的时间花在哪里：线性层（和上下文长度无关）还是注意力（随上下文变长）----
    std::printf("\nDecode 一步的耗时随上下文长度的变化：\n");
    std::printf("%12s %12s\n", "上下文长度", "TPOT ms");
    {
        const std::vector<int> all = randomTokens(1000, cfg.vocab, 9);
        KvCache cache(cfg);
        int fed = 0;
        for (int target : {1, 250, 500, 1000}) {
            // 把缓存填到 target-8 个位置，再测 8 步 Decode
            const std::vector<int> chunk(all.begin() + fed, all.begin() + std::max(fed, target - 8));
            if (!chunk.empty()) {
                const Mat ignored = forwardCached(model, cache, chunk);
                inftut::keep(ignored.v[0]);
                fed += static_cast<int>(chunk.size());
            }
            std::vector<double> stepMs;
            while (fed < target) {
                stepMs.push_back(inftut::onceMs([&] {
                    const Mat logits = forwardCached(model, cache, {all[static_cast<std::size_t>(fed)]});
                    inftut::keep(logits.v[0]);
                }));
                ++fed;
            }
            std::sort(stepMs.begin(), stepMs.end());
            std::printf("%12d %12.4f\n", target, stepMs[stepMs.size() / 2]);
        }
    }
    return 0;
}
