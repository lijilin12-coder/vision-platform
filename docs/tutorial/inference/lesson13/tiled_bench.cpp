// 第 13 课示例三：序列变长时，标准做法和分块做法的临时内存与耗时。
//
// 一定要用优化模式编译：
//   macOS / Linux：clang++ -std=c++17 -O2 -I../common tiled_bench.cpp -o tiled_bench && ./tiled_bench
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 /I..\common tiled_bench.cpp && tiled_bench.exe
#include <cstdio>
#include <vector>

#include "attention.h"
#include "inftut/common.h"

int main() {
    const int d = 64;
    const int block = 64;
    std::printf("单头注意力，d = %d，分块大小 %d × %d\n\n", d, block, block);
    std::printf("%8s | %14s %10s | %14s %10s | %10s\n", "N", "标准 临时内存", "耗时 ms", "分块 临时内存", "耗时 ms", "最大误差");

    bool ok = true;
    for (int n : {256, 512, 1024, 2048}) {
        inftut::Rng rng(5);
        std::vector<float> q;
        std::vector<float> k;
        std::vector<float> v;
        for (int i = 0; i < n * d; ++i) {
            q.push_back(rng.normal());
            k.push_back(rng.normal());
            v.push_back(rng.normal());
        }
        attn::Result standard;
        attn::Result tiled;
        const int repeats = 5;
        const double standardMs = inftut::medianMs(repeats, [&] { standard = attn::attentionStandard(q, k, v, n, d); });
        const double tiledMs = inftut::medianMs(repeats, [&] { tiled = attn::attentionTiled(q, k, v, n, d, block, block); });
        const double diff = inftut::maxAbsDiff(standard.out, tiled.out);
        std::printf("%8d | %11.2f MB %10.1f | %11.2f MB %10.1f | %10.2e\n", n, static_cast<double>(standard.scratchBytes) / 1048576.0,
                    standardMs, static_cast<double>(tiled.scratchBytes) / 1048576.0, tiledMs, diff);
        ok = ok && diff < 1e-4;
    }

    // 块的大小对耗时的影响
    {
        const int n = 1024;
        inftut::Rng rng(5);
        std::vector<float> q;
        std::vector<float> k;
        std::vector<float> v;
        for (int i = 0; i < n * d; ++i) {
            q.push_back(rng.normal());
            k.push_back(rng.normal());
            v.push_back(rng.normal());
        }
        std::printf("\nN = %d，不同的分块大小：\n%12s %16s %10s\n", n, "块", "临时内存 字节", "耗时 ms");
        for (int b : {1, 8, 64, 1024}) {
            attn::Result tiled;
            const double ms = inftut::medianMs(5, [&] { tiled = attn::attentionTiled(q, k, v, n, d, b, b); });
            std::printf("%5d × %-5d %16zu %10.1f\n", b, b, tiled.scratchBytes, ms);
        }
    }

    if (!ok) {
        std::printf("错误：分块做法和标准做法的结果不一致\n");
        return 1;
    }
    return 0;
}
