// 量化的基本工具：量化参数、量化与反量化、校准。
//
// 量化就是用整数 q 近似表示实数 r：
//     r ≈ scale × (q − zeroPoint)
// scale 是相邻两个整数代表的实数间隔；zeroPoint 是“实数 0”对应的那个整数。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace quant {

struct QParams {
    float scale = 1.0f;
    int zeroPoint = 0;
};

struct Range {
    float lo = 0;
    float hi = 0;
};

// ---------- 选择量化参数 ----------

// 非对称量化，用 uint8（0～255）：把 [lo, hi] 均匀地映射到 [0, 255]。常用于激活值。
// 区间必须包含 0：补边、ReLU 都要用到“恰好等于 0”的值，它必须能被精确表示。
inline QParams chooseAffine(Range r) {
    const float lo = std::min(r.lo, 0.0f);
    const float hi = std::max(r.hi, 0.0f);
    QParams p;
    p.scale = (hi - lo) / 255.0f;
    if (p.scale <= 0) p.scale = 1.0f;  // 全是 0 的情况
    p.zeroPoint = static_cast<int>(std::lround(-lo / p.scale));
    return p;
}

// 对称量化，用有符号整数：把 [-maxAbs, maxAbs] 映射到 [-qmax, qmax]，zeroPoint 固定是 0。常用于权重。
// bits = 8 时 qmax = 127；bits = 4 时 qmax = 7。
inline int qmaxOf(int bits) { return (1 << (bits - 1)) - 1; }

inline float chooseSymmetricScale(float maxAbs, int bits = 8) {
    return maxAbs > 0 ? maxAbs / static_cast<float>(qmaxOf(bits)) : 1.0f;
}

// ---------- 量化与反量化 ----------

inline std::uint8_t quantizeU8(float r, QParams p) {
    const long q = std::lround(r / p.scale) + p.zeroPoint;
    return static_cast<std::uint8_t>(std::clamp<long>(q, 0, 255));  // 超出范围的被“截断”到边界上
}

inline std::int8_t quantizeS8(float r, float scale, int bits = 8) {
    const long q = std::lround(r / scale);
    return static_cast<std::int8_t>(std::clamp<long>(q, -qmaxOf(bits), qmaxOf(bits)));
}

inline float dequantize(int q, QParams p) { return p.scale * static_cast<float>(q - p.zeroPoint); }

// ---------- 校准：根据一批真实数据确定取值范围 ----------

// 最大最小值法：范围就是见过的最小值和最大值。
inline Range minMax(const std::vector<float>& values) {
    const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
    return Range{*lo, *hi};
}

// 百分位法：丢掉两头各 (100 − percent)% 的数据。percent = 99.9 时，范围是第 0.1 百分位到第 99.9 百分位。
inline Range percentile(std::vector<float> values, double percent) {
    std::sort(values.begin(), values.end());
    const double tail = (100.0 - percent) / 100.0;
    const auto last = static_cast<double>(values.size() - 1);
    const auto loIndex = static_cast<std::size_t>(std::floor(tail * last));
    const auto hiIndex = static_cast<std::size_t>(std::ceil((1.0 - tail) * last));
    return Range{values[loIndex], values[hiIndex]};
}

// ---------- 衡量量化误差 ----------

// 信号与量化噪声之比（SQNR），单位分贝：10·log10(Σ r² / Σ (r − r')²)。越大越好，每多 1 位大约多 6 分贝。
inline double sqnrDb(const std::vector<float>& original, const std::vector<float>& restored) {
    double signal = 0, noise = 0;
    for (std::size_t i = 0; i < original.size(); ++i) {
        const double d = static_cast<double>(original[i]) - static_cast<double>(restored[i]);
        signal += static_cast<double>(original[i]) * static_cast<double>(original[i]);
        noise += d * d;
    }
    return noise > 0 ? 10.0 * std::log10(signal / noise) : 999.0;
}

}  // namespace quant
