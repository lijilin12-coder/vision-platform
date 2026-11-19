// 矩阵乘法的五种写法：朴素 → 交换循环顺序 → SIMD → 分块 → 多线程。每一步都测量，并检查结果没变。
//
// 一定要用优化模式编译，并允许编译器使用本机 CPU 的指令：
//   macOS / Linux：clang++ -std=c++17 -O2 -march=native -I../common matmul.cpp -o matmul -pthread && ./matmul
//   Windows：      cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 /I..\common matmul.cpp && matmul.exe
//
// 朴素写法在 1024 × 1024 上要好几秒，默认跳过。想看的话：./matmul --naive
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// 打印 name 并用空格补到 width 列宽。一个汉字占 2 列、在 UTF-8 里占 3 个字节，printf 的 %-20s 对不齐。
void printPadded(const char* name, int width) {
    int columns = 0;
    for (const char* p = name; *p != '\0'; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (ch < 0x80) columns += 1;            // ASCII 字符
        else if ((ch & 0xC0) != 0x80) columns += 2;  // 一个多字节字符的第一个字节
    }
    std::printf("  %s%*s", name, columns < width ? width - columns : 0, "");
}

// 测量一种写法：打印耗时和 GFLOPS，并和参考结果比较。返回耗时（毫秒）。
template <typename Fn>
double report(const char* name, int n, int repeats, const std::vector<float>& reference, const std::vector<float>& c,
              Fn&& fn) {
    const double ms = inftut::medianMs(repeats, fn);
    // 一次乘法加一次加法算 2 次浮点运算，一共 n³ 组。GFLOPS：每秒多少个“十亿次浮点运算”
    const double gflops = 2.0 * n * n * n / (ms * 1e-3) / 1e9;
    const double diff = inftut::maxAbsDiff(reference, c);
    printPadded(name, 26);
    std::printf("%9.2f ms  %7.2f GFLOPS   最大误差 %.1e\n", ms, gflops, diff);
    // float 的加法顺序不同，结果会差一点点；差得多就是写错了
    if (!(diff < 1e-2)) {
        std::printf("    结果和参考结果不一致！\n");
        ++failures;
    }
    return ms;
}

void run(int n, int repeats, bool withNaive) {
    const std::vector<float> a = randomMatrix(n, n, 1);
    const std::vector<float> b = randomMatrix(n, n, 2);
    std::vector<float> reference(static_cast<std::size_t>(n) * n);
    std::vector<float> c(reference.size());
    const int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::printf("%d × %d 的 float 矩阵相乘（每个矩阵 %.2f MB），每项取 %d 次的中位数：\n", n, n,
                n * n * 4 / 1048576.0, repeats);
    double first = 0;
    if (withNaive) {
        gemm::naive(a.data(), b.data(), reference.data(), n, n, n);
        first = report("1. 朴素（i-j-k）", n, repeats, reference, c,
                       [&] { gemm::naive(a.data(), b.data(), c.data(), n, n, n); });
    } else {
        gemm::reordered(a.data(), b.data(), reference.data(), n, n, n);
        printPadded("1. 朴素（i-j-k）", 26);
        std::printf("（跳过，加 --naive 运行；下面的误差是和第 2 种比）\n");
    }
    const double second = report("2. 交换顺序（i-k-j）", n, repeats, reference, c,
                                 [&] { gemm::reordered(a.data(), b.data(), c.data(), n, n, n); });
    if (!withNaive) first = second;
    report("3. SIMD", n, repeats, reference, c, [&] { gemm::simd(a.data(), b.data(), c.data(), n, n, n); });
    report("4. SIMD + 分块", n, repeats, reference, c, [&] { gemm::blocked(a.data(), b.data(), c.data(), n, n, n); });
    char name[64];
    std::snprintf(name, sizeof(name), "5. SIMD + 分块 + %d 线程", threads);
    const double last = report(name, n, repeats, reference, c,
                               [&] { gemm::threaded(a.data(), b.data(), c.data(), n, n, n, threads); });
    std::printf("  第 5 种比第 %d 种快 %.0f 倍\n\n", withNaive ? 1 : 2, first / last);
}

}  // namespace

int main(int argc, char** argv) {
    const bool naiveOnLarge = argc > 1 && std::strcmp(argv[1], "--naive") == 0;
    std::printf("SIMD 指令集：%s\n\n", GEMM_SIMD_NAME);
    run(256, 5, true);            // B 有 0.25 MB：放得进二级缓存
    run(1024, 3, naiveOnLarge);   // B 有 4 MB：放不进
    if (failures > 0) {
        std::printf("有 %d 项结果不一致\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("各种写法的结果一致\n");
    return EXIT_SUCCESS;
}
