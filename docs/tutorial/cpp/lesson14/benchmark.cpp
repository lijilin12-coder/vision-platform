// 性能分析入门：先测量，再优化。对比三组“结果相同、速度不同”的写法。
//
// 一定要用优化模式编译，否则测到的数字没有意义：
//   macOS / Linux：clang++ -std=c++17 -O2 benchmark.cpp -o benchmark && ./benchmark
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 benchmark.cpp && benchmark.exe
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

// ---------- 测量工具 ----------

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

// 把结果“用掉”，防止编译器发现结果没人用而把整段计算删掉。
volatile std::uint64_t sink = 0;

// ---------- 对比一：遍历顺序 ----------

struct Gray {
    int width;
    int height;
    std::vector<std::uint8_t> pixels;  // 按行存放：第 y 行第 x 列在 pixels[y * width + x]
};

std::uint64_t sumRowByRow(const Gray& img) {
    std::uint64_t sum = 0;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) sum += img.pixels[static_cast<std::size_t>(y) * img.width + x];
    return sum;
}

std::uint64_t sumColumnByColumn(const Gray& img) {
    std::uint64_t sum = 0;
    for (int x = 0; x < img.width; ++x)
        for (int y = 0; y < img.height; ++y) sum += img.pixels[static_cast<std::size_t>(y) * img.width + x];
    return sum;
}

// ---------- 对比二：传值还是传引用 ----------

std::uint64_t firstPixelByValue(Gray img) { return img.pixels[0]; }           // 每次调用复制整张图
std::uint64_t firstPixelByRef(const Gray& img) { return img.pixels[0]; }      // 不复制

// ---------- 对比三：预留空间 ----------

std::vector<int> collectWithoutReserve(int n) {
    std::vector<int> out;
    for (int i = 0; i < n; ++i) out.push_back(i);
    return out;
}

std::vector<int> collectWithReserve(int n) {
    std::vector<int> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) out.push_back(i);
    return out;
}

void report(const std::string& name, double slowMs, double fastMs) {
    std::cout << "  " << name << "：慢 " << slowMs << " ms，快 " << fastMs << " ms，相差 "
              << (fastMs > 0 ? slowMs / fastMs : 0) << " 倍\n";
}

int main() {
    // 一张 5MP 的灰度图
    Gray img{2592, 1944, std::vector<std::uint8_t>(2592u * 1944u)};
    std::iota(img.pixels.begin(), img.pixels.end(), static_cast<std::uint8_t>(0));

    // 两种遍历顺序的结果必须相同：优化不能改变结果
    const std::uint64_t expected = sumRowByRow(img);
    if (sumColumnByColumn(img) != expected) {
        std::cout << "两种遍历顺序的结果不一致\n";
        return EXIT_FAILURE;
    }
    if (collectWithReserve(1000) != collectWithoutReserve(1000)) return EXIT_FAILURE;

    std::cout << "5MP 灰度图（2592×1944），每项重复 15 次取中位数：\n";

    const double byColumn = medianMs(15, [&] { sink = sumColumnByColumn(img); });
    const double byRow = medianMs(15, [&] { sink = sumRowByRow(img); });
    report("逐列遍历 / 逐行遍历", byColumn, byRow);

    const double byValue = medianMs(15, [&] { sink = firstPixelByValue(img); });
    const double byRef = medianMs(15, [&] { sink = firstPixelByRef(img); });
    report("传值 / 传 const 引用", byValue, byRef);

    const double noReserve = medianMs(15, [] { sink = collectWithoutReserve(5000000).size(); });
    const double withReserve = medianMs(15, [] { sink = collectWithReserve(5000000).size(); });
    report("不预留 / 预留空间（500 万个 int）", noReserve, withReserve);

    return EXIT_SUCCESS;
}
