// 算子：每个算子是一个普通函数，输入张量，输出张量。全部是最朴素的写法，只求看得懂、算得对。
//
// 每个算子有两个版本：
//   Tensor relu(const Tensor& x);              返回一个新张量，写起来方便（第 1～3 课用）
//   void reluInto(const Tensor& x, Tensor& y); 把结果写进调用者给的张量（第 4 课做内存规划时用）
// 前一个只是后一个的包装。
#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

#include "tensor.h"

namespace inftut {

// ---------- 卷积 ----------
// x：输入，N×Cin×H×W
// weight：卷积核，Cout×Cin×Kh×Kw（借用 Tensor 的四个维度：n 是输出通道数，c 是输入通道数）
// bias：每个输出通道一个数，1×Cout×1×1
// stride：步长；pad：四周补零的圈数
// relu：算完顺手把负数改成 0（第 3 课的“算子融合”用，平时是 false）
inline void conv2dInto(const Tensor& x, const Tensor& weight, const Tensor& bias, int stride, int pad, bool relu,
                       Tensor& y) {
    assert(&x != &y);  // 卷积不能原地做：算后面的输出还要用到前面的输入
    assert(weight.c == x.c && bias.c == weight.n && stride >= 1 && pad >= 0);
    const int outC = weight.n;
    const int outH = (x.h + 2 * pad - weight.h) / stride + 1;
    const int outW = (x.w + 2 * pad - weight.w) / stride + 1;
    y.resize(x.n, outC, outH, outW);

    for (int n = 0; n < x.n; ++n)
        for (int oc = 0; oc < outC; ++oc)
            for (int oy = 0; oy < outH; ++oy)
                for (int ox = 0; ox < outW; ++ox) {
                    // 一个输出元素 = 卷积核盖住的那一小块输入，逐个相乘再相加
                    float sum = bias.data[static_cast<std::size_t>(oc)];
                    for (int ic = 0; ic < x.c; ++ic)
                        for (int ky = 0; ky < weight.h; ++ky) {
                            const int iy = oy * stride - pad + ky;
                            if (iy < 0 || iy >= x.h) continue;  // 落在补零的区域：乘以 0，跳过
                            for (int kx = 0; kx < weight.w; ++kx) {
                                const int ix = ox * stride - pad + kx;
                                if (ix < 0 || ix >= x.w) continue;
                                sum += x.at(n, ic, iy, ix) * weight.at(oc, ic, ky, kx);
                            }
                        }
                    if (relu && sum < 0) sum = 0;
                    y.at(n, oc, oy, ox) = sum;
                }
}

inline Tensor conv2d(const Tensor& x, const Tensor& weight, const Tensor& bias, int stride, int pad) {
    Tensor y;
    conv2dInto(x, weight, bias, stride, pad, false, y);
    return y;
}

// ---------- ReLU：负数变成 0 ----------
// 每个输出元素只依赖同一位置的输入元素，所以 y 和 x 可以是同一个张量（原地）。
inline void reluInto(const Tensor& x, Tensor& y) {
    y.resize(x.n, x.c, x.h, x.w);
    for (std::size_t i = 0; i < x.data.size(); ++i) y.data[i] = std::max(0.0f, x.data[i]);
}

inline Tensor relu(const Tensor& x) {
    Tensor y;
    reluInto(x, y);
    return y;
}

// ---------- 最大池化：每 k×k 的小块里取最大值，宽和高各缩小到 1/k ----------
inline void maxPoolInto(const Tensor& x, int k, Tensor& y) {
    assert(&x != &y && k >= 1);
    y.resize(x.n, x.c, x.h / k, x.w / k);
    for (int n = 0; n < y.n; ++n)
        for (int c = 0; c < y.c; ++c)
            for (int oy = 0; oy < y.h; ++oy)
                for (int ox = 0; ox < y.w; ++ox) {
                    float best = x.at(n, c, oy * k, ox * k);
                    for (int ky = 0; ky < k; ++ky)
                        for (int kx = 0; kx < k; ++kx) best = std::max(best, x.at(n, c, oy * k + ky, ox * k + kx));
                    y.at(n, c, oy, ox) = best;
                }
}

inline Tensor maxPool(const Tensor& x, int k) {
    Tensor y;
    maxPoolInto(x, k, y);
    return y;
}

// ---------- 全连接 ----------
// 把每张图的 C×H×W 个数看成一个长向量 v，输出 out[j] = bias[j] + Σ_i weight[j][i] * v[i]。
// weight：Out×In×1×1（In 必须等于 C*H*W）；bias：1×Out×1×1；输出：N×Out×1×1
inline void linearInto(const Tensor& x, const Tensor& weight, const Tensor& bias, Tensor& y) {
    assert(&x != &y);
    const std::size_t in = static_cast<std::size_t>(x.c) * static_cast<std::size_t>(x.h) * static_cast<std::size_t>(x.w);
    assert(static_cast<std::size_t>(weight.c) == in && bias.c == weight.n);
    y.resize(x.n, weight.n, 1, 1);
    for (int n = 0; n < x.n; ++n) {
        const float* v = x.data.data() + static_cast<std::size_t>(n) * in;
        for (int j = 0; j < weight.n; ++j) {
            const float* row = weight.data.data() + static_cast<std::size_t>(j) * in;
            float sum = bias.data[static_cast<std::size_t>(j)];
            for (std::size_t i = 0; i < in; ++i) sum += row[i] * v[i];
            y.at(n, j, 0, 0) = sum;
        }
    }
}

inline Tensor linear(const Tensor& x, const Tensor& weight, const Tensor& bias) {
    Tensor y;
    linearInto(x, weight, bias, y);
    return y;
}

// ---------- Softmax：把一组任意大小的数变成加起来等于 1 的“概率” ----------
// 对每张图的 C 个数做。可以原地。
inline void softmaxInto(const Tensor& x, Tensor& y) {
    assert(x.h == 1 && x.w == 1);
    y.resize(x.n, x.c, 1, 1);
    const std::size_t c = static_cast<std::size_t>(x.c);
    for (int n = 0; n < x.n; ++n) {
        const std::size_t base = static_cast<std::size_t>(n) * c;
        // 先减去最大值再取指数：结果不变，但不会因为 exp(很大的数) 而溢出
        float biggest = x.data[base];
        for (std::size_t i = 1; i < c; ++i) biggest = std::max(biggest, x.data[base + i]);
        float total = 0;
        for (std::size_t i = 0; i < c; ++i) {
            y.data[base + i] = std::exp(x.data[base + i] - biggest);
            total += y.data[base + i];
        }
        for (std::size_t i = 0; i < c; ++i) y.data[base + i] /= total;
    }
}

inline Tensor softmax(const Tensor& x) {
    Tensor y;
    softmaxInto(x, y);
    return y;
}

// ---------- 下面两个算子第 1 课不用，第 2～4 课的网络里会出现 ----------

// 逐元素相加（两个张量形状相同）。可以原地。
inline void addInto(const Tensor& a, const Tensor& b, Tensor& y) {
    assert(a.sameShape(b));
    y.resize(a.n, a.c, a.h, a.w);
    for (std::size_t i = 0; i < a.data.size(); ++i) y.data[i] = a.data[i] + b.data[i];
}

inline Tensor add(const Tensor& a, const Tensor& b) {
    Tensor y;
    addInto(a, b, y);
    return y;
}

// 批归一化（推理时）：每个通道做一次线性变换 y = gamma * (x - mean) / sqrt(var + eps) + beta。
// gamma、beta、mean、var 都是 1×C×1×1，训练时得到，推理时是常数。可以原地。
constexpr float kBatchNormEps = 1e-5f;

inline void batchNormInto(const Tensor& x, const Tensor& gamma, const Tensor& beta, const Tensor& mean,
                          const Tensor& var, Tensor& y) {
    assert(gamma.c == x.c && beta.c == x.c && mean.c == x.c && var.c == x.c);
    y.resize(x.n, x.c, x.h, x.w);
    const std::size_t plane = static_cast<std::size_t>(x.h) * static_cast<std::size_t>(x.w);
    for (int n = 0; n < x.n; ++n)
        for (int c = 0; c < x.c; ++c) {
            const std::size_t ch = static_cast<std::size_t>(c);
            const float scale = gamma.data[ch] / std::sqrt(var.data[ch] + kBatchNormEps);
            const float shift = beta.data[ch] - mean.data[ch] * scale;
            const std::size_t base = (static_cast<std::size_t>(n) * static_cast<std::size_t>(x.c) + ch) * plane;
            for (std::size_t i = 0; i < plane; ++i) y.data[base + i] = x.data[base + i] * scale + shift;
        }
}

inline Tensor batchNorm(const Tensor& x, const Tensor& gamma, const Tensor& beta, const Tensor& mean,
                        const Tensor& var) {
    Tensor y;
    batchNormInto(x, gamma, beta, mean, var, y);
    return y;
}

// ---------- 生成随机张量（教程里的“权重”都是这样来的）----------
// 元素服从均值 0、标准差 stddev 的近似正态分布。rng 是 inftut::Rng（见 common/inftut/common.h）。
template <typename RngT>
Tensor randomTensor(RngT& rng, int n, int c, int h, int w, float stddev) {
    Tensor t(n, c, h, w);
    for (float& v : t.data) v = rng.normal() * stddev;
    return t;
}

}  // namespace inftut
