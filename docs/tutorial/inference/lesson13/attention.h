// 同一个注意力计算的两种写法：
//   attentionStandard：先算出完整的 N × N 打分矩阵，再整体 softmax，再乘 V（框架里“三个矩阵运算”的写法）
//   attentionTiled：   分块计算，用在线 softmax 逐块累加，不保存 N × N 矩阵（FlashAttention 的核心思路）
// 单头、带因果掩码（第 i 个位置只看第 0..i 个位置）。Q、K、V 都是 N × d，按行存放。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace attn {

struct Result {
    std::vector<float> out;        // N × d
    std::size_t scratchBytes = 0;  // 计算过程中用到的临时数组一共多少字节（不算输入和输出）
};

inline float dot(const float* a, const float* b, int n) {
    float sum = 0;
    for (int i = 0; i < n; ++i) sum += a[i] * b[i];
    return sum;
}

inline Result attentionStandard(const std::vector<float>& q, const std::vector<float>& k, const std::vector<float>& v, int n, int d) {
    const float scale = 1.0f / std::sqrt(static_cast<float>(d));
    const std::size_t N = static_cast<std::size_t>(n);
    const std::size_t D = static_cast<std::size_t>(d);
    Result r;
    r.out.assign(N * D, 0.0f);

    // 第一步：S = Q · Kᵀ，N × N。被掩码挡住的位置（j > i）填 -∞
    std::vector<float> s(N * N);
    r.scratchBytes = s.size() * sizeof(float);
    for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t j = 0; j < N; ++j) {
            s[i * N + j] = j <= i ? dot(&q[i * D], &k[j * D], d) * scale : -INFINITY;
        }
    }
    // 第二步：对 S 的每一行做 softmax，结果仍然是 N × N（这里直接写回 S）
    for (std::size_t i = 0; i < N; ++i) {
        float* row = &s[i * N];
        float maxScore = row[0];
        for (std::size_t j = 1; j < N; ++j) maxScore = std::max(maxScore, row[j]);
        float sum = 0;
        for (std::size_t j = 0; j < N; ++j) {
            row[j] = std::exp(row[j] - maxScore);   // exp(-∞) = 0
            sum += row[j];
        }
        for (std::size_t j = 0; j < N; ++j) row[j] /= sum;
    }
    // 第三步：O = P · V
    for (std::size_t i = 0; i < N; ++i) {
        float* out = &r.out[i * D];
        for (std::size_t j = 0; j <= i; ++j) {   // j > i 的权重是 0，跳过
            const float w = s[i * N + j];
            const float* value = &v[j * D];
            for (std::size_t c = 0; c < D; ++c) out[c] += w * value[c];
        }
    }
    return r;
}

// blockQ：一次处理多少个查询（行）；blockK：一次处理多少个键（列）
inline Result attentionTiled(const std::vector<float>& q, const std::vector<float>& k, const std::vector<float>& v, int n, int d,
                             int blockQ, int blockK) {
    const float scale = 1.0f / std::sqrt(static_cast<float>(d));
    const std::size_t D = static_cast<std::size_t>(d);
    Result r;
    r.out.assign(static_cast<std::size_t>(n) * D, 0.0f);

    // 临时空间只有这么多，和 N 无关
    std::vector<float> s(static_cast<std::size_t>(blockQ) * static_cast<std::size_t>(blockK));   // 当前这一小块的打分
    std::vector<float> m(static_cast<std::size_t>(blockQ));                                      // 每个查询：见过的最大打分
    std::vector<float> l(static_cast<std::size_t>(blockQ));                                      // 每个查询：Σ exp(s - m)
    r.scratchBytes = (s.size() + m.size() + l.size()) * sizeof(float);

    for (int i0 = 0; i0 < n; i0 += blockQ) {
        const int rows = std::min(blockQ, n - i0);
        std::fill(m.begin(), m.end(), -INFINITY);
        std::fill(l.begin(), l.end(), 0.0f);
        // 输出的这几行就当作在线 softmax 里的 o，直接在上面累加

        // 因果掩码：这一块查询最多看到第 i0 + rows - 1 个键，更靠后的键块整块跳过
        for (int j0 = 0; j0 < i0 + rows; j0 += blockK) {
            const int cols = std::min(blockK, n - j0);
            // 1. 这一小块的打分：rows × cols
            for (int a = 0; a < rows; ++a) {
                const float* query = &q[static_cast<std::size_t>(i0 + a) * D];
                float* srow = &s[static_cast<std::size_t>(a) * static_cast<std::size_t>(blockK)];
                for (int b = 0; b < cols; ++b) {
                    srow[b] = j0 + b <= i0 + a ? dot(query, &k[static_cast<std::size_t>(j0 + b) * D], d) * scale : -INFINITY;
                }
            }
            // 2. 在线 softmax：每个查询更新自己的 m、l、o
            for (int a = 0; a < rows; ++a) {
                const float* srow = &s[static_cast<std::size_t>(a) * static_cast<std::size_t>(blockK)];
                float mNew = m[static_cast<std::size_t>(a)];
                for (int b = 0; b < cols; ++b) mNew = std::max(mNew, srow[b]);
                if (mNew == -INFINITY) continue;   // 这个查询在这一块里一个键都看不到
                const float rescale = std::exp(m[static_cast<std::size_t>(a)] - mNew);
                float* out = &r.out[static_cast<std::size_t>(i0 + a) * D];
                float lNew = l[static_cast<std::size_t>(a)] * rescale;
                if (rescale != 1.0f) {
                    for (std::size_t c = 0; c < D; ++c) out[c] *= rescale;
                }
                for (int b = 0; b < cols; ++b) {
                    const float w = std::exp(srow[b] - mNew);
                    if (w == 0.0f) continue;
                    lNew += w;
                    const float* value = &v[static_cast<std::size_t>(j0 + b) * D];
                    for (std::size_t c = 0; c < D; ++c) out[c] += w * value[c];
                }
                m[static_cast<std::size_t>(a)] = mNew;
                l[static_cast<std::size_t>(a)] = lNew;
            }
        }
        // 3. 这一块查询看完了所有的键：o / l 就是结果
        for (int a = 0; a < rows; ++a) {
            float* out = &r.out[static_cast<std::size_t>(i0 + a) * D];
            const float inv = 1.0f / l[static_cast<std::size_t>(a)];
            for (std::size_t c = 0; c < D; ++c) out[c] *= inv;
        }
    }
    return r;
}

}  // namespace attn
