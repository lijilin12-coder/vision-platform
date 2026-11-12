// 为什么需要线程：两种完全不同的理由。
//
//   1. 计算型任务：用多个核心同时算，缩短总时间。线程数超过核心数之后不会更快。
//   2. 等待型任务：一个线程在等（等相机、等磁盘、等网络）的时候，CPU 是空闲的，可以去做别的事。
//      这种情况线程数可以远多于核心数。
//
// 一定要用优化模式编译，否则数字没有参考价值：
//   macOS / Linux：clang++ -std=c++17 -O2 -pthread why_threads.cpp -o why_threads && ./why_threads
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 why_threads.cpp && why_threads.exe
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// ---------- 计算型：纯粹消耗 CPU ----------

// 对 [begin, end) 里的每个数做一点运算再求和。结果只取决于范围，和怎么分给线程无关。
std::uint64_t compute(std::uint64_t begin, std::uint64_t end) {
    std::uint64_t sum = 0;
    for (std::uint64_t i = begin; i < end; ++i) {
        std::uint64_t x = i * 2654435761u;
        x ^= x >> 13;
        sum += x % 1000;
    }
    return sum;
}

// 把总的工作量平均分给 threadCount 个线程。每个线程写自己的那个结果，互不干扰，所以不需要锁。
std::uint64_t computeWithThreads(std::uint64_t total, int threadCount) {
    std::vector<std::uint64_t> partial(static_cast<std::size_t>(threadCount), 0);
    std::vector<std::thread> threads;
    const std::uint64_t chunk = total / static_cast<std::uint64_t>(threadCount);
    for (int t = 0; t < threadCount; ++t) {
        const std::uint64_t begin = chunk * static_cast<std::uint64_t>(t);
        const std::uint64_t end = t == threadCount - 1 ? total : begin + chunk;
        threads.emplace_back([&partial, t, begin, end] { partial[static_cast<std::size_t>(t)] = compute(begin, end); });
    }
    for (auto& th : threads) th.join();
    std::uint64_t sum = 0;
    for (const auto p : partial) sum += p;
    return sum;
}

// ---------- 等待型：大部分时间在等，几乎不用 CPU ----------

void waitForCamera() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }  // 模拟等相机出图

int main() {
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    std::cout << "逻辑核心数：" << cores << "\n\n";

    // 1. 计算型
    const std::uint64_t total = 400000000;
    const std::uint64_t expected = compute(0, total);
    std::cout << "计算型任务（同样的总工作量，分给不同数量的线程）：\n";
    double oneThreadMs = 0;
    bool allCorrect = true;
    for (const int n : {1, 2, 4, 8, 16}) {
        const auto start = Clock::now();
        const std::uint64_t result = computeWithThreads(total, n);
        const double ms = msSince(start);
        if (n == 1) oneThreadMs = ms;
        allCorrect = allCorrect && result == expected;
        std::cout << "  " << n << " 个线程：" << ms << " ms，是单线程的 " << oneThreadMs / ms << " 倍速\n";
    }

    // 2. 等待型
    std::cout << "\n等待型任务（8 台相机，每台等 50 ms）：\n";
    {
        const auto start = Clock::now();
        for (int i = 0; i < 8; ++i) waitForCamera();
        std::cout << "  一个线程轮流等：" << msSince(start) << " ms\n";
    }
    {
        const auto start = Clock::now();
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) threads.emplace_back(waitForCamera);
        for (auto& th : threads) th.join();
        std::cout << "  8 个线程同时等：" << msSince(start) << " ms\n";
    }

    std::cout << "\n" << (allCorrect ? "无论分给几个线程，计算结果都相同\n" : "计算结果不一致\n");
    return allCorrect ? EXIT_SUCCESS : EXIT_FAILURE;
}
