// 第 13 课示例二：分块注意力的结果必须和标准做法一致，不管块取多大。
//
//   macOS / Linux：clang++ -std=c++17 -I../common tiled_test.cpp -o tiled_test && ./tiled_test
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common tiled_test.cpp && tiled_test.exe
#include <cstdio>
#include <vector>

#include "attention.h"
#include "inftut/common.h"

int main() {
    const int n = 100;   // 故意不是块大小的整数倍
    const int d = 16;
    inftut::Rng rng(5);
    std::vector<float> q;
    std::vector<float> k;
    std::vector<float> v;
    for (int i = 0; i < n * d; ++i) {
        q.push_back(rng.normal() * 2.0f);   // 乘 2 让打分的范围大一些，考验数值稳定性
        k.push_back(rng.normal() * 2.0f);
        v.push_back(rng.normal());
    }

    const attn::Result standard = attn::attentionStandard(q, k, v, n, d);
    std::printf("N = %d，d = %d。标准做法的临时内存：%zu 字节（N × N × 4）\n\n", n, d, standard.scratchBytes);
    std::printf("%8s %8s %14s %14s\n", "查询块", "键块", "临时内存 字节", "最大误差");

    bool ok = standard.scratchBytes == static_cast<std::size_t>(n) * static_cast<std::size_t>(n) * sizeof(float);
    const int blocks[][2] = {{1, 1}, {1, 100}, {8, 8}, {16, 32}, {7, 13}, {100, 100}, {128, 128}};
    for (const auto& b : blocks) {
        const attn::Result tiled = attn::attentionTiled(q, k, v, n, d, b[0], b[1]);
        const double diff = inftut::maxAbsDiff(tiled.out, standard.out);
        std::printf("%8d %8d %14zu %14.2e\n", b[0], b[1], tiled.scratchBytes, diff);
        ok = ok && diff < 1e-5;
    }
    if (!ok) {
        std::printf("错误：分块做法和标准做法的结果不一致\n");
        return 1;
    }
    return 0;
}
