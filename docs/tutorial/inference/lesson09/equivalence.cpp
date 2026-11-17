// 第 9 课示例一：带 KV Cache 的前向计算，结果必须和不带缓存的完全一样。
//
//   macOS / Linux：clang++ -std=c++17 -I../common equivalence.cpp -o equivalence && ./equivalence
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common equivalence.cpp && equivalence.exe
#include <iostream>
#include <vector>

#include "kv_cache.h"

using namespace tinygpt;

namespace {

// 两种算法做的是同样的运算、同样的顺序，在大多数编译器上误差就是 0。
// 留一点余量：有的编译器在不同的地方会用不同的指令做同一个运算，产生最后一位上的差别。
const double kTolerance = 1e-4;

// 取出 logits 的第 r 行
std::vector<float> rowOf(const Mat& m, int r) { return std::vector<float>(m.row(r), m.row(r) + m.cols); }

}  // namespace

int main() {
    Config cfg;  // 词表 64，隐藏维度 32，4 个头，2 层
    const Model model = makeModel(cfg, 42);
    const std::vector<int> tokens = {5, 17, 3, 42, 8, 21, 57, 17, 39, 18, 54, 25};
    const int n = static_cast<int>(tokens.size());
    bool ok = true;

    // 标准答案：不带缓存，12 个 token 一次算完
    const Mat reference = forward(model, tokens);

    // ---- 方式一：一个 token 一个 token 地喂（全程 Decode）----
    {
        KvCache cache(cfg);
        double worst = 0;
        for (int i = 0; i < n; ++i) {
            const Mat logits = forwardCached(model, cache, {tokens[static_cast<std::size_t>(i)]});
            worst = std::max(worst, inftut::maxAbsDiff(rowOf(logits, 0), rowOf(reference, i)));
        }
        std::cout << "逐个 token 喂入，和不带缓存相比，logits 的最大误差：" << worst << "\n";
        ok = ok && worst < kTolerance;
    }

    // ---- 方式二：前 8 个 token 一次喂入（Prefill），后 4 个逐个喂入（Decode）----
    {
        KvCache cache(cfg);
        const std::vector<int> prompt(tokens.begin(), tokens.begin() + 8);
        const Mat prefill = forwardCached(model, cache, prompt);
        double worst = 0;
        for (int i = 0; i < 8; ++i) worst = std::max(worst, inftut::maxAbsDiff(rowOf(prefill, i), rowOf(reference, i)));
        for (int i = 8; i < n; ++i) {
            const Mat logits = forwardCached(model, cache, {tokens[static_cast<std::size_t>(i)]});
            worst = std::max(worst, inftut::maxAbsDiff(rowOf(logits, 0), rowOf(reference, i)));
        }
        std::cout << "Prefill 8 个 + Decode 4 个，和不带缓存相比，logits 的最大误差：" << worst << "\n";
        ok = ok && worst < kTolerance;

        // ---- 缓存占多少内存：2（K 和 V）× 层数 × token 数 × 隐藏维度 × 每个数的字节数 ----
        const std::size_t expected = 2u * static_cast<std::size_t>(cfg.layers) * static_cast<std::size_t>(n) *
                                     static_cast<std::size_t>(cfg.dim) * sizeof(float);
        std::cout << "\n缓存了 " << cache.length << " 个位置，占 " << cache.bytes() << " 字节；公式算出来是 2 × " << cfg.layers
                  << " × " << n << " × " << cfg.dim << " × " << sizeof(float) << " = " << expected << " 字节\n";
        ok = ok && cache.bytes() == expected && cache.length == n;

        // ---- 截断：丢掉最后 4 个位置，再重新喂一遍，结果应该不变 ----
        cache.truncate(8);
        double worst2 = 0;
        for (int i = 8; i < n; ++i) {
            const Mat logits = forwardCached(model, cache, {tokens[static_cast<std::size_t>(i)]});
            worst2 = std::max(worst2, inftut::maxAbsDiff(rowOf(logits, 0), rowOf(reference, i)));
        }
        std::cout << "截断到 8 个位置后重新 Decode，最大误差：" << worst2 << "\n";
        ok = ok && worst2 < kTolerance;
    }

    // ---- 生成：两种方式生成的 token 应该一模一样 ----
    {
        const std::vector<int> prompt = {5, 17, 3, 42, 8, 21};
        const std::vector<int> a = generateGreedy(model, prompt, 20);
        const std::vector<int> b = generateGreedyCached(model, prompt, 20);
        std::cout << "\n贪心生成 20 个 token，带缓存和不带缓存的结果" << (a == b ? "相同" : "不同") << "：";
        for (int t : b) std::cout << t << " ";
        std::cout << "\n";
        ok = ok && a == b;
    }

    if (!ok) {
        std::cout << "错误：带缓存和不带缓存的结果不一致\n";
        return 1;
    }
    return 0;
}
