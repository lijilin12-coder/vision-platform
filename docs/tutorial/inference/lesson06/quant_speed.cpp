// INT8 比 FP32 快吗？测一测“矩阵 × 向量”（全连接层的核心）。
//
// 结论先说：整数并不天然比浮点快。快的原因有两个，都要靠 SIMD 才能兑现：
//   1. 一个数只占 1 个字节，同样宽的 SIMD 寄存器一次能装下 4 倍的数；
//   2. 数据量是 1/4，读内存的时间也是 1/4。
//
//   macOS / Linux：clang++ -std=c++17 -O2 -march=native -I../common quant_speed.cpp -o quant_speed && ./quant_speed
//   Windows：      cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 /I..\common quant_speed.cpp && quant_speed.exe
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "inftut/common.h"

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#define SPEED_SIMD_NAME "AVX2 + FMA"
#define SPEED_AVX2 1
#else
#define SPEED_SIMD_NAME "无（SIMD 版本退回成普通循环，两行数字会差不多）"
#endif

namespace {

// ---------- FP32：y[o] = Σ w[o][i] * x[i] ----------

float dotFloatScalar(const float* w, const float* x, int n) {
    float sum = 0;
    for (int i = 0; i < n; ++i) sum += w[i] * x[i];
    return sum;
}

float dotFloatSimd(const float* w, const float* x, int n) {
#if defined(SPEED_AVX2)
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {  // 一趟 16 个 float，用两个累加器
        acc0 = _mm256_fmadd_ps(_mm256_loadu_ps(w + i), _mm256_loadu_ps(x + i), acc0);
        acc1 = _mm256_fmadd_ps(_mm256_loadu_ps(w + i + 8), _mm256_loadu_ps(x + i + 8), acc1);
    }
    float parts[8];
    _mm256_storeu_ps(parts, _mm256_add_ps(acc0, acc1));
    float sum = 0;
    for (float p : parts) sum += p;
    for (; i < n; ++i) sum += w[i] * x[i];
    return sum;
#else
    return dotFloatScalar(w, x, n);
#endif
}

// ---------- INT8：acc[o] = Σ qw[o][i] * qx[i]，权重是 int8，输入是 uint8，累加到 int32 ----------

std::int32_t dotInt8Scalar(const std::int8_t* w, const std::uint8_t* x, int n) {
    std::int32_t sum = 0;
    for (int i = 0; i < n; ++i) sum += static_cast<std::int32_t>(w[i]) * static_cast<std::int32_t>(x[i]);
    return sum;
}

std::int32_t dotInt8Simd(const std::int8_t* w, const std::uint8_t* x, int n) {
#if defined(SPEED_AVX2)
    __m256i acc = _mm256_setzero_si256();
    int i = 0;
    for (; i + 32 <= n; i += 32) {  // 一趟 32 个数
        // 先把 8 位扩展成 16 位（16 个一组），再用 madd：相邻两个 16 位乘积相加成一个 32 位。结果是精确的。
        const __m256i w0 = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(w + i)));
        const __m256i x0 = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i)));
        const __m256i w1 = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(w + i + 16)));
        const __m256i x1 = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(x + i + 16)));
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(w0, x0));
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(w1, x1));
    }
    std::int32_t parts[8];
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(parts), acc);
    std::int32_t sum = 0;
    for (std::int32_t p : parts) sum += p;
    for (; i < n; ++i) sum += static_cast<std::int32_t>(w[i]) * static_cast<std::int32_t>(x[i]);
    return sum;
#else
    return dotInt8Scalar(w, x, n);
#endif
}

template <typename W, typename X, typename Y, typename Dot>
void matVec(const std::vector<W>& w, const std::vector<X>& x, std::vector<Y>& y, int rows, int cols, Dot dot) {
    for (int o = 0; o < rows; ++o) y[static_cast<std::size_t>(o)] = dot(w.data() + static_cast<std::size_t>(o) * static_cast<std::size_t>(cols), x.data(), cols);
}

int failures = 0;

void run(int rows, int cols, int repeats) {
    inftut::Rng rng(5);
    const std::size_t count = static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);
    std::vector<float> wf(count), xf(static_cast<std::size_t>(cols));
    std::vector<std::int8_t> wq(count);
    std::vector<std::uint8_t> xq(static_cast<std::size_t>(cols));
    for (std::size_t i = 0; i < count; ++i) {
        wf[i] = rng.uniform(-1.0f, 1.0f);
        wq[i] = static_cast<std::int8_t>(static_cast<int>(rng.below(255)) - 127);
    }
    for (std::size_t i = 0; i < xf.size(); ++i) {
        xf[i] = rng.uniform(0.0f, 1.0f);
        xq[i] = static_cast<std::uint8_t>(rng.below(256));
    }
    std::vector<float> yf(static_cast<std::size_t>(rows)), yfSimd(yf);
    std::vector<std::int32_t> yq(static_cast<std::size_t>(rows)), yqSimd(yq);

    const double tFloat = inftut::medianMs(repeats, [&] { matVec(wf, xf, yf, rows, cols, dotFloatScalar); });
    const double tFloatSimd = inftut::medianMs(repeats, [&] { matVec(wf, xf, yfSimd, rows, cols, dotFloatSimd); });
    const double tInt = inftut::medianMs(repeats, [&] { matVec(wq, xq, yq, rows, cols, dotInt8Scalar); });
    const double tIntSimd = inftut::medianMs(repeats, [&] { matVec(wq, xq, yqSimd, rows, cols, dotInt8Simd); });

    std::printf("%d × %d 的矩阵乘以向量（FP32 权重 %.1f MB，INT8 权重 %.1f MB），取 %d 次的中位数：\n", rows, cols,
                static_cast<double>(count) * 4 / 1048576.0, static_cast<double>(count) / 1048576.0, repeats);
    std::printf("  FP32，普通循环   %8.3f ms\n", tFloat);
    std::printf("  INT8，普通循环   %8.3f ms   是 FP32 普通循环的 %.1f 倍快\n", tInt, tFloat / tInt);
    std::printf("  FP32，SIMD       %8.3f ms\n", tFloatSimd);
    std::printf("  INT8，SIMD       %8.3f ms   是 FP32 SIMD 的 %.1f 倍快\n\n", tIntSimd, tFloatSimd / tIntSimd);

    if (yq != yqSimd) ++failures, std::printf("INT8 的两种写法结果不同！\n");       // 整数运算：必须逐位相同
    if (!(inftut::maxAbsDiff(yf, yfSimd) < 1e-2)) ++failures, std::printf("FP32 的两种写法结果相差太大！\n");
}

}  // namespace

int main() {
    std::printf("SIMD 指令集：%s\n\n", SPEED_SIMD_NAME);
    run(512, 512, 51);     // FP32 权重 1 MB：放得进二级缓存
    run(4096, 4096, 11);   // FP32 权重 64 MB：放不进
    if (failures > 0) return EXIT_FAILURE;
    std::printf("INT8 的两种写法结果逐位相同；FP32 的两种写法只差在加法顺序上\n");
    return EXIT_SUCCESS;
}
