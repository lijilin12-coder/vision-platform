// 矩阵乘法 C = A × B 的五种写法：朴素 → 交换循环顺序 → SIMD → 分块 → 多线程。结果相同，速度相差上百倍。
//
// 矩阵都按行存放在一段连续的 float 里：M 行 N 列的矩阵，第 i 行第 j 列在 data[i * N + j]。
//   A：M × K      B：K × N      C：M × N
// 深度学习里把这个运算叫 GEMM（general matrix multiply），这是线性代数库 BLAS 里的名字。
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

// ---- 选择 SIMD 指令集。没有合适的指令集时用普通的标量代码，结果相同，只是慢 ----
// MSVC 打开 /arch:AVX2 时只定义 __AVX2__，不定义 __FMA__，但可以使用 FMA 指令
#if defined(__AVX2__) && (defined(__FMA__) || defined(_MSC_VER))
#include <immintrin.h>
#define GEMM_SIMD_NAME "AVX2 + FMA（一次 8 个 float）"
#define GEMM_SIMD_AVX2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define GEMM_SIMD_NAME "NEON（一次 4 个 float）"
#define GEMM_SIMD_NEON 1
#else
#define GEMM_SIMD_NAME "无（标量回退）"
#endif

namespace gemm {

// 1. 朴素写法：照着数学定义写。c[i][j] = Σ a[i][k] * b[k][j]
inline void naive(const float* a, const float* b, float* c, int M, int K, int N) {
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            float sum = 0;
            for (int k = 0; k < K; ++k) sum += a[i * K + k] * b[k * N + j];  // b 是按列跳着访问的
            c[i * N + j] = sum;
        }
    }
}

// 2. 交换循环顺序（i-k-j）：最内层循环里 b 和 c 都是按行连续访问
inline void reordered(const float* a, const float* b, float* c, int M, int K, int N) {
    std::memset(c, 0, sizeof(float) * static_cast<std::size_t>(M) * N);
    for (int i = 0; i < M; ++i) {
        for (int k = 0; k < K; ++k) {
            const float aik = a[i * K + k];
            const float* brow = b + k * N;
            float* crow = c + i * N;
            for (int j = 0; j < N; ++j) crow[j] += aik * brow[j];
        }
    }
}

// c 的一行 += aik * b 的一行（标量）。k 的个数不是 4 的倍数时，SIMD 版本用它处理剩下的。
inline void axpyScalar(float aik, const float* brow, float* crow, int n) {
    for (int j = 0; j < n; ++j) crow[j] += aik * brow[j];
}

// 3. SIMD：一条指令处理 c 的一行里相邻的 8 个（或 4 个）数，同时把 4 个 k 合并成一趟
//    crow[j] += a0*b0[j] + a1*b1[j] + a2*b2[j] + a3*b3[j]
//    这样 crow[j] 只读一次、写一次，而不是四次。
inline void axpy4(const float* a4, const float* b0, const float* b1, const float* b2, const float* b3,
                  float* crow, int n) {
    int j = 0;
#if defined(GEMM_SIMD_AVX2)
    const __m256 va0 = _mm256_set1_ps(a4[0]);
    const __m256 va1 = _mm256_set1_ps(a4[1]);
    const __m256 va2 = _mm256_set1_ps(a4[2]);
    const __m256 va3 = _mm256_set1_ps(a4[3]);
    for (; j + 8 <= n; j += 8) {
        __m256 acc = _mm256_loadu_ps(crow + j);
        acc = _mm256_fmadd_ps(va0, _mm256_loadu_ps(b0 + j), acc);  // acc += va0 * b0[j..j+7]
        acc = _mm256_fmadd_ps(va1, _mm256_loadu_ps(b1 + j), acc);
        acc = _mm256_fmadd_ps(va2, _mm256_loadu_ps(b2 + j), acc);
        acc = _mm256_fmadd_ps(va3, _mm256_loadu_ps(b3 + j), acc);
        _mm256_storeu_ps(crow + j, acc);
    }
#elif defined(GEMM_SIMD_NEON)
    const float32x4_t va0 = vdupq_n_f32(a4[0]);
    const float32x4_t va1 = vdupq_n_f32(a4[1]);
    const float32x4_t va2 = vdupq_n_f32(a4[2]);
    const float32x4_t va3 = vdupq_n_f32(a4[3]);
    for (; j + 4 <= n; j += 4) {
        float32x4_t acc = vld1q_f32(crow + j);
        acc = vfmaq_f32(acc, va0, vld1q_f32(b0 + j));  // acc += va0 * b0[j..j+3]
        acc = vfmaq_f32(acc, va1, vld1q_f32(b1 + j));
        acc = vfmaq_f32(acc, va2, vld1q_f32(b2 + j));
        acc = vfmaq_f32(acc, va3, vld1q_f32(b3 + j));
        vst1q_f32(crow + j, acc);
    }
#endif
    for (; j < n; ++j) crow[j] += a4[0] * b0[j] + a4[1] * b1[j] + a4[2] * b2[j] + a4[3] * b3[j];  // 剩下不足一组的
}

inline void simd(const float* a, const float* b, float* c, int M, int K, int N) {
    std::memset(c, 0, sizeof(float) * static_cast<std::size_t>(M) * N);
    for (int i = 0; i < M; ++i) {
        float* crow = c + i * N;
        int k = 0;
        for (; k + 4 <= K; k += 4) {
            axpy4(a + i * K + k, b + k * N, b + (k + 1) * N, b + (k + 2) * N, b + (k + 3) * N, crow, N);
        }
        for (; k < K; ++k) axpyScalar(a[i * K + k], b + k * N, crow, N);
    }
}

// 4. 分块：把 k 和 j 两个方向切成小块。最里面三层循环只碰 B 的一个 kBlockK × kBlockJ 的小块，
//    它有 64 × 256 × 4 字节 = 64 KB，算 C 的每一行时都要用到它，所以它一直留在缓存里。
constexpr int kBlockK = 64;
constexpr int kBlockJ = 256;

// 只计算 C 的第 [rowBegin, rowEnd) 行。多线程版本让每个线程算其中一段。
inline void blockedRows(const float* a, const float* b, float* c, int K, int N, int rowBegin, int rowEnd) {
    std::memset(c + static_cast<std::size_t>(rowBegin) * N, 0,
                sizeof(float) * static_cast<std::size_t>(rowEnd - rowBegin) * N);
    for (int k0 = 0; k0 < K; k0 += kBlockK) {
        const int k1 = std::min(k0 + kBlockK, K);
        for (int j0 = 0; j0 < N; j0 += kBlockJ) {
            const int j1 = std::min(j0 + kBlockJ, N);
            for (int i = rowBegin; i < rowEnd; ++i) {
                float* crow = c + i * N + j0;
                int k = k0;
                for (; k + 4 <= k1; k += 4) {
                    axpy4(a + i * K + k, b + k * N + j0, b + (k + 1) * N + j0, b + (k + 2) * N + j0,
                          b + (k + 3) * N + j0, crow, j1 - j0);
                }
                for (; k < k1; ++k) axpyScalar(a[i * K + k], b + k * N + j0, crow, j1 - j0);
            }
        }
    }
}

inline void blocked(const float* a, const float* b, float* c, int M, int K, int N) {
    blockedRows(a, b, c, K, N, 0, M);
}

// 5. 多线程：C 的每一行互不相干，把行平均分给几个线程。各线程写的是 C 的不同行，不需要锁。
inline void threaded(const float* a, const float* b, float* c, int M, int K, int N, int threads) {
    threads = std::max(1, std::min(threads, M));
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        const int begin = M * t / threads;
        const int end = M * (t + 1) / threads;
        workers.emplace_back([=] { blockedRows(a, b, c, K, N, begin, end); });
    }
    for (auto& w : workers) w.join();
}

}  // namespace gemm
