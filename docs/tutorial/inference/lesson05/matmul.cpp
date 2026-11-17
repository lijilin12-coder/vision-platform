// 矩阵乘法的五种写法：朴素 → 交换循环顺序 → 分块 → SIMD → 多线程。每一步都测量，并检查结果没变。
//
// 一定要用优化模式编译：
//   macOS / Linux：clang++ -std=c++17 -O2 -march=native -I../common matmul.cpp -o matmul -pthread && ./matmul
//   Windows：      cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 /I..\common matmul.cpp && matmul.exe
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "gemm.h"
#include "inftut/common.h"

namespace {

std::vector<float> randomMatrix(int rows, int cols, std::uint64_t seed) {
    inftut::Rng rng(seed);
    std::vector<float> m(static_cast<std::size_t>(rows) * cols);
    for (float& v : m) v = rng.uniform(-1.0f, 1.0f);
    return m;
}

int failures = 0;

// 测量一种写法：打印耗时、GFLOPS、相对朴素写法的倍数，并和参考结果比较。
template <typename Fn>
double report(const char* name, int n, int repeats, const std::vector<float>& reference, std::vector<float>& c,
              double baselineMs, Fn&& fn) {
    const double ms = inftut::medianMs(repeats, fn);
    inftut::keep(c[0]);
    // 一次乘法加一次加法算 2 次浮点运算，一共 n³ 组
    const double gflops = 2.0 * n * n * n / (ms * 1e-3) / 1e9;
    const double diff = inftut::maxAbsDiff(reference, c);
    std::printf("  %-22s %9.2f ms  %7.2f GFLOPS  %6.1f 倍   最大误差 %.1e\n", name, ms, gflops,
                baselineMs > 0 ? baselineMs / ms : 1.0, diff);
    // float 的加法顺序不同，结果会差一点点；差得多就是写错了
    if (!(diff < 1e-2)) {
        std::printf("    结果和朴素写法不一致！\n");
        ++failures;
    }
    return ms;
}

void run(int n, int repeats) {
    const std::vector<float> a = randomMatrix(n, n, 1);
    const std::vector<float> b = randomMatrix(n, n, 2);
    std::vector<float> reference(static_cast<std::size_t>(n) * n);
    std::vector<float> c(reference.size());
    const int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::printf("%d × %d 的 float 矩阵相乘（三个矩阵各 %.1f MB），每项取 %d 次的中位数：\n", n, n,
                n * n * 4 / 1048576.0, repeats);
    gemm::naive(a.data(), b.data(), reference.data(), n, n, n);
    const double base = report("1. 朴素（i-j-k）", n, repeats, reference, c, 0,
                               [&] { gemm::naive(a.data(), b.data(), c.data(), n, n, n); });
    report("2. 交换顺序（i-k-j）", n, repeats, reference, c, base,
           [&] { gemm::reordered(a.data(), b.data(), c.data(), n, n, n); });
    report("3. 分块", n, repeats, reference, c, base, [&] { gemm::blocked(a.data(), b.data(), c.data(), n, n, n); });
    report("4. 分块 + SIMD", n, repeats, reference, c, base,
           [&] { gemm::simd(a.data(), b.data(), c.data(), n, n, n); });
    char name[64];
    std::snprintf(name, sizeof(name), "5. 再加 %d 个线程", threads);
    report(name, n, repeats, reference, c, base,
           [&] { gemm::threaded(a.data(), b.data(), c.data(), n, n, n, threads); });
    std::printf("\n");
}

}  // namespace

int main() {
    std::printf("SIMD 指令集：%s\n\n", GEMM_SIMD_NAME);
    run(256, 5);    // 三个矩阵加起来 0.75 MB：放得进二级缓存
    run(1024, 3);   // 三个矩阵加起来 12 MB：放不进
    if (failures > 0) {
        std::printf("有 %d 项结果不一致\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("五种写法的结果一致\n");
    return EXIT_SUCCESS;
}
