// 为什么需要线程池：同样的一批小任务，三种做法各用多少时间。
//
//   macOS / Linux：clang++ -std=c++17 -O2 -pthread pool_vs_spawn.cpp -o pool_vs_spawn && ./pool_vs_spawn
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 pool_vs_spawn.cpp && pool_vs_spawn.exe
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

#include "thread_pool.h"

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// 一个小任务：大约几十微秒的计算。
std::uint64_t smallTask(std::uint64_t seed) {
    std::uint64_t x = seed;
    for (int i = 0; i < 20000; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        x ^= x >> 29;
    }
    return x % 1000;
}

int main() {
    const int taskCount = 4000;
    const std::size_t cores = std::thread::hardware_concurrency();
    std::cout << "逻辑核心数：" << cores << "，任务数：" << taskCount << "\n\n";

    // 1. 一个线程按顺序做
    auto start = Clock::now();
    std::uint64_t expected = 0;
    for (int i = 0; i < taskCount; ++i) expected += smallTask(static_cast<std::uint64_t>(i));
    const double sequentialMs = msSince(start);
    std::cout << "按顺序做：            " << sequentialMs << " ms（每个任务约 " << sequentialMs * 1000 / taskCount
              << " 微秒）\n";

    // 2. 每个任务开一个线程
    start = Clock::now();
    std::uint64_t spawnSum = 0;
    {
        std::vector<std::uint64_t> results(static_cast<std::size_t>(taskCount));
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(taskCount));
        for (int i = 0; i < taskCount; ++i) {
            threads.emplace_back([&results, i] {
                results[static_cast<std::size_t>(i)] = smallTask(static_cast<std::uint64_t>(i));
            });
        }
        for (auto& t : threads) t.join();
        for (const auto r : results) spawnSum += r;
    }
    const double spawnMs = msSince(start);
    std::cout << "每个任务开一个线程：  " << spawnMs << " ms\n";

    // 3. 线程池：线程只创建一次，反复使用
    std::uint64_t poolSum = 0;
    double poolMs = 0;
    {
        ThreadPool pool(cores);
        start = Clock::now();
        std::vector<std::future<std::uint64_t>> futures;
        futures.reserve(static_cast<std::size_t>(taskCount));
        for (int i = 0; i < taskCount; ++i) {
            futures.push_back(pool.submit([i] { return smallTask(static_cast<std::uint64_t>(i)); }));
        }
        for (auto& f : futures) poolSum += f.get();
        poolMs = msSince(start);
    }
    std::cout << "线程池（" << cores << " 个线程）：  " << poolMs << " ms\n\n";

    std::cout << "线程池比按顺序做快 " << sequentialMs / poolMs << " 倍，比每个任务开一个线程快 " << spawnMs / poolMs
              << " 倍\n";

    const bool same = spawnSum == expected && poolSum == expected;
    std::cout << (same ? "三种做法的结果相同\n" : "结果不一致\n");
    return same ? EXIT_SUCCESS : EXIT_FAILURE;
}
