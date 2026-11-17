// 整套教程共用的小工具：计时、防止被优化掉、可重复的随机数、浮点比较。
// 只有头文件，只用标准库。用法：#include "inftut/common.h"
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace inftut {

// ---------- 计时（C++ 教程第 14 课）----------

// 把 fn 运行 repeats 次，返回耗时的中位数（毫秒）。
// 用中位数而不是平均值：偶尔一次被系统打断的慢结果不会把数字带偏。
template <typename Fn>
double medianMs(int repeats, Fn&& fn) {
    using Clock = std::chrono::steady_clock;  // 单调时钟：不受系统校时影响
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(repeats));
    for (int i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        fn();
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

// 只运行一次，返回耗时（毫秒）。
template <typename Fn>
double onceMs(Fn&& fn) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// 把结果“用掉”，防止编译器发现结果没人用而把整段计算删掉。
// 用法：inftut::keep(sum);
inline void keep(double value) {
    static volatile double sink = 0;
    sink = value;
}

// ---------- 可重复的随机数 ----------

// 标准库的 std::normal_distribution 在不同编译器上给出的数不一样。
// 教程里的“权重”都是随机生成的，为了让三个系统上的输出完全相同，这里自己实现一个。
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed * 2654435769ULL + 1442695040888963407ULL) {}

    // 均匀分布的 32 位整数（PCG 的简化版）
    std::uint32_t nextU32() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state_ >> 33) ^ static_cast<std::uint32_t>(state_ >> 13);
    }

    // [0, 1) 上的均匀分布
    float uniform() { return static_cast<float>(nextU32() >> 8) * (1.0f / 16777216.0f); }

    // [lo, hi) 上的均匀分布
    float uniform(float lo, float hi) { return lo + (hi - lo) * uniform(); }

    // 近似的标准正态分布：12 个均匀分布的和减 6（均值 0，方差 1）。
    // 只用加法，所以在所有平台上结果逐位相同。
    float normal() {
        float sum = 0;
        for (int i = 0; i < 12; ++i) sum += uniform();
        return sum - 6.0f;
    }

    // [0, n) 上的均匀整数
    std::uint32_t below(std::uint32_t n) { return nextU32() % n; }

private:
    std::uint64_t state_;
};

// ---------- 比较两组浮点数 ----------

// 返回两组数里对应元素之差的最大绝对值。长度不同时返回无穷大。
inline double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return INFINITY;
    double worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
    return worst;
}

}  // namespace inftut
