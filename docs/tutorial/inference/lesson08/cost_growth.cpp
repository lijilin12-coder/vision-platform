// 第 8 课示例二：不带缓存时，生成第 n 个 token 要把前面 n 个 token 全部重新算一遍。
// 测量一次前向计算的耗时随 token 数的变化。
//
// 一定要用优化模式编译：
//   macOS / Linux：clang++ -std=c++17 -O2 -I../common cost_growth.cpp -o cost_growth && ./cost_growth
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 /I..\common cost_growth.cpp && cost_growth.exe
#include <cstdio>
#include <vector>

#include "tiny_gpt.h"

using namespace tinygpt;

int main() {
    Config cfg;
    cfg.vocab = 1000;
    cfg.dim = 128;
    cfg.heads = 4;
    cfg.layers = 4;
    cfg.maxSeq = 512;
    const Model model = makeModel(cfg, 42);
    std::printf("模型：隐藏维度 %d，%d 层，%d 个头，词表 %d，共 %.2f 百万个参数\n\n", cfg.dim, cfg.layers, cfg.heads,
                cfg.vocab, static_cast<double>(parameterCount(model)) / 1e6);

    inftut::Rng rng(7);
    std::vector<int> tokens;
    for (int i = 0; i < cfg.maxSeq; ++i) tokens.push_back(static_cast<int>(rng.below(static_cast<std::uint32_t>(cfg.vocab))));

    std::printf("已有 n 个 token 时，生成下一个 token 的耗时（一次完整的前向计算）：\n");
    std::printf("%8s %12s %16s\n", "n", "耗时 ms", "每个 token ms");
    double cumulative = 0;   // 估算：从 1 个 token 一路生成到 n 个，总共要多久
    int previousN = 0;
    double previousMs = 0;
    for (int n : {16, 32, 64, 128, 256, 512}) {
        const std::vector<int> prefix(tokens.begin(), tokens.begin() + n);
        const double ms = inftut::medianMs(5, [&] {
            const Mat logits = forward(model, prefix);
            inftut::keep(logits.row(logits.rows - 1)[0]);
        });
        std::printf("%8d %12.2f %16.4f\n", n, ms, ms / n);
        // 梯形法估算 previousN..n 这一段每一步耗时的总和
        cumulative += 0.5 * (previousMs + ms) * (n - previousN);
        previousN = n;
        previousMs = ms;
    }
    std::printf("\n估算：不带缓存，从头连续生成 512 个 token，总共约 %.1f 秒\n", cumulative / 1000.0);
    std::printf("如果每个 token 只算一次（下一课的 KV Cache），512 个 token 约 %.2f 秒\n", previousMs / 1000.0);
    return 0;
}
