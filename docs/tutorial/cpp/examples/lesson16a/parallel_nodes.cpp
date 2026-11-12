// 用线程池并行执行“互不依赖的节点”。
//
// 一次检测里，四个节点都只读同一张图像，彼此不需要对方的结果，所以可以同时执行。
// 对比三种做法，结果必须完全相同：
//   1. 按顺序执行
//   2. 四个节点各作为一个任务交给线程池（任务并行）
//   3. 在 2 的基础上，把最慢的那个节点按行拆成几块（数据并行）
//
//   macOS / Linux：clang++ -std=c++17 -O2 -pthread parallel_nodes.cpp -o parallel_nodes && ./parallel_nodes
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 parallel_nodes.cpp && parallel_nodes.exe
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "thread_pool.h"

using Clock = std::chrono::steady_clock;

struct Gray {
    int width;
    int height;
    std::vector<std::uint8_t> pixels;
    std::uint8_t at(int x, int y) const { return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)]; }
};

// ---------- 四个互不依赖的“节点”：都只读图像，各自算出一个数 ----------

// 亮斑：灰度超过阈值的像素数
std::int64_t countBright(const Gray& img) {
    std::int64_t n = 0;
    for (const auto p : img.pixels) n += p > 200 ? 1 : 0;
    return n;
}

// 直方图：出现次数最多的灰度值
std::int64_t dominantGray(const Gray& img) {
    std::array<std::int64_t, 256> histogram{};
    for (const auto p : img.pixels) ++histogram[p];
    return std::max_element(histogram.begin(), histogram.end()) - histogram.begin();
}

// 边缘强度：水平方向相邻像素差的绝对值之和
std::int64_t edgeStrength(const Gray& img) {
    std::int64_t sum = 0;
    for (int y = 0; y < img.height; ++y)
        for (int x = 1; x < img.width; ++x) sum += std::abs(static_cast<int>(img.at(x, y)) - static_cast<int>(img.at(x - 1, y)));
    return sum;
}

// 污渍：3×3 平均之后仍然偏暗的像素数（四个里最慢的一个）。只处理第 yBegin 到 yEnd 行。
// 每一行的结果只取决于它和上下两行的像素，行与行之间互不影响，所以可以把图像按行分块，各算各的。
std::int64_t countDarkAfterBlur(const Gray& img, int yBegin, int yEnd) {
    std::int64_t n = 0;
    for (int y = std::max(yBegin, 1); y < std::min(yEnd, img.height - 1); ++y) {
        for (int x = 1; x < img.width - 1; ++x) {
            int sum = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) sum += img.at(x + dx, y + dy);
            n += sum / 9 < 40 ? 1 : 0;
        }
    }
    return n;
}

std::int64_t countDarkAfterBlur(const Gray& img) { return countDarkAfterBlur(img, 0, img.height); }

using Results = std::array<std::int64_t, 4>;

Results runSequentially(const Gray& img) {
    return {countBright(img), dominantGray(img), edgeStrength(img), countDarkAfterBlur(img)};
}

Results runInPool(ThreadPool& pool, const Gray& img) {
    // 按引用捕获 img 是安全的：下面马上 get()，在所有任务完成之前，这个函数不会返回，img 一直有效。
    // 四个任务只读 img、各写各的返回值，没有任何共享的可变数据，所以不需要锁。
    auto bright = pool.submit([&img] { return countBright(img); });
    auto gray = pool.submit([&img] { return dominantGray(img); });
    auto edge = pool.submit([&img] { return edgeStrength(img); });
    auto dark = pool.submit([&img] { return countDarkAfterBlur(img); });
    return {bright.get(), gray.get(), edge.get(), dark.get()};
}

// 在上面的基础上，把最慢的节点按行分成 blocks 块，每块一个任务。
Results runInPoolSplittingSlowNode(ThreadPool& pool, const Gray& img, int blocks) {
    auto bright = pool.submit([&img] { return countBright(img); });
    auto gray = pool.submit([&img] { return dominantGray(img); });
    auto edge = pool.submit([&img] { return edgeStrength(img); });

    std::vector<std::future<std::int64_t>> parts;
    for (int b = 0; b < blocks; ++b) {
        const int yBegin = img.height * b / blocks;
        const int yEnd = img.height * (b + 1) / blocks;
        parts.push_back(pool.submit([&img, yBegin, yEnd] { return countDarkAfterBlur(img, yBegin, yEnd); }));
    }
    std::int64_t dark = 0;
    for (auto& part : parts) dark += part.get();
    return {bright.get(), gray.get(), edge.get(), dark};
}

// 把结果“用掉”，防止编译器发现结果没人用而把计算删掉（第 14 课）。
volatile std::int64_t sink = 0;

template <typename Fn>
double medianMs(int repeats, Fn&& fn) {
    std::vector<double> times;
    for (int i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        fn();
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

int main() {
    // 一张 5MP 的测试图像
    Gray img{2592, 1944, std::vector<std::uint8_t>(2592u * 1944u)};
    std::uint32_t state = 12345;
    for (auto& p : img.pixels) {
        state = state * 1664525u + 1013904223u;
        p = static_cast<std::uint8_t>(state >> 24);
    }

    // 每个节点单独的耗时
    const std::array<std::string, 4> names{"亮斑计数", "直方图", "边缘强度", "模糊后暗区"};
    const std::array<double, 4> each{
        medianMs(7, [&] { sink = countBright(img); }),
        medianMs(7, [&] { sink = dominantGray(img); }),
        medianMs(7, [&] { sink = edgeStrength(img); }),
        medianMs(7, [&] { sink = countDarkAfterBlur(img); }),
    };
    double total = 0, longest = 0;
    std::cout << "各节点单独的耗时：\n";
    for (std::size_t i = 0; i < 4; ++i) {
        std::cout << "  " << names[i] << "：" << each[i] << " ms\n";
        total += each[i];
        longest = std::max(longest, each[i]);
    }

    std::cout << "  合计：" << total << " ms，其中最慢的一个：" << longest << " ms\n";

    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    ThreadPool pool(static_cast<std::size_t>(cores));
    Results sequential{}, parallel{}, split{};
    const double sequentialMs = medianMs(7, [&] { sequential = runSequentially(img); });
    const double parallelMs = medianMs(7, [&] { parallel = runInPool(pool, img); });
    const double splitMs = medianMs(7, [&] { split = runInPoolSplittingSlowNode(pool, img, cores); });

    std::cout << "\n1. 按顺序执行四个节点：            " << sequentialMs << " ms\n";
    std::cout << "2. 四个节点交给线程池：            " << parallelMs << " ms（快 " << sequentialMs / parallelMs
              << " 倍）  ← 不可能比最慢的那个节点更短\n";
    std::cout << "3. 再把最慢的节点按行分成 " << cores << " 块： " << splitMs << " ms（快 " << sequentialMs / splitMs
              << " 倍）\n";

    const bool same = sequential == parallel && sequential == split;
    std::cout << (same ? "\n三种做法的结果完全相同\n" : "\n结果不一致\n");
    return same ? EXIT_SUCCESS : EXIT_FAILURE;
}
