// 使用线程池时最常见的四个坑。每一个都做成了可以安全运行的演示（不会真的卡死）。
//
//   macOS / Linux：clang++ -std=c++17 -O2 -pthread pitfalls.cpp -o pitfalls && ./pitfalls
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 pitfalls.cpp && pitfalls.exe
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "thread_pool.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

// ---------- 坑一：任务里抛出的异常 ----------
// 异常不会让程序崩溃，也不会自己冒出来：它被存进了 future，只有调用 get() 的人才会收到。
// 如果没有人调用 get()，这个错误就无声无息地消失了。
void exceptionInTask() {
    std::cout << "坑一：任务里抛出的异常\n";
    ThreadPool pool(2);

    auto failing = pool.submit([]() -> int { throw std::runtime_error("节点执行失败"); });
    bool caught = false;
    try {
        (void)failing.get();
    } catch (const std::runtime_error& e) {
        caught = std::string(e.what()) == "节点执行失败";
    }
    check(caught, "调用 get() 的线程收到了任务里的异常");

    auto ignored = pool.submit([] { throw std::runtime_error("没有人会知道这个错误"); });
    ignored.wait();  // 只等它结束，不取结果：异常被悄悄丢掉了
    std::cout << "  第二个任务也抛了异常，但没有人调用 get()：程序照常运行，错误无声无息\n";

    auto next = pool.submit([] { return 42; });
    check(next.get() == 42, "工作线程没有被异常弄坏，后面的任务照常执行");
}

// ---------- 坑二：池里的任务等待同一个池里的另一个任务 ----------
// 只有 1 个工作线程。外层任务占着它，又去等内层任务；内层任务排在队列里，等这个线程空出来。
// 两边互相等，永远等不到。这里用带超时的等待来演示，所以不会真的卡死。
void taskWaitingOnSamePool() {
    std::cout << "坑二：池里的任务等待同一个池里的另一个任务\n";
    ThreadPool pool(1);

    auto outer = pool.submit([&pool] {
        auto inner = pool.submit([] { return 1; });           // 内层任务进了队列
        const auto status = inner.wait_for(200ms);            // 唯一的工作线程正被自己占着
        return status == std::future_status::timeout;          // 如果这里写的是 inner.get()，就永远卡住了
    });
    const bool timedOut = outer.get();
    check(timedOut, "内层任务在外层任务等待期间始终没有机会执行");
    std::cout << "  外层任务等了 200 ms，内层任务一直没有开始：如果用的是 get()，整个线程池就卡死了\n";
}

// ---------- 坑三：会长时间等待的任务占满了线程池 ----------
// 2 个工作线程，先来了 2 个要等很久的任务（比如等相机、等网络）。
// 之后来的计算任务明明只要一瞬间，却要排队等前面的任务结束。
void blockingTasksStarveOthers() {
    std::cout << "坑三：会长时间等待的任务占满了线程池\n";
    ThreadPool pool(2);

    auto slow1 = pool.submit([] { std::this_thread::sleep_for(300ms); });
    auto slow2 = pool.submit([] { std::this_thread::sleep_for(300ms); });

    const auto start = Clock::now();
    auto quick = pool.submit([] { return 6 * 7; });  // 本身只需要几纳秒
    const int answer = quick.get();
    const double waited = msSince(start);

    slow1.get();
    slow2.get();
    check(answer == 42, "计算任务的结果正确");
    check(waited > 200, "计算任务被迫等了很久");
    std::cout << "  一个瞬间就能算完的任务，等了 " << waited << " ms 才拿到结果\n";
}

// ---------- 坑四：任务切得太碎 ----------
// 把 400 万个数求和。任务切得越碎，花在“提交任务、交接、取结果”上的时间就越多。
void tasksTooSmall() {
    std::cout << "坑四：任务切得太碎\n";
    std::vector<std::uint32_t> numbers(4000000);
    std::iota(numbers.begin(), numbers.end(), 1u);
    const std::uint64_t expected = std::accumulate(numbers.begin(), numbers.end(), std::uint64_t{0});

    auto start = Clock::now();
    const std::uint64_t direct = std::accumulate(numbers.begin(), numbers.end(), std::uint64_t{0});
    const double directMs = msSince(start);
    check(direct == expected, "直接求和");
    std::cout << "  不用线程池，直接求和：        " << directMs << " ms\n";

    ThreadPool pool(std::thread::hardware_concurrency());
    for (const std::size_t chunk : {std::size_t{100}, std::size_t{10000}, std::size_t{1000000}}) {
        start = Clock::now();
        std::vector<std::future<std::uint64_t>> parts;
        for (std::size_t begin = 0; begin < numbers.size(); begin += chunk) {
            const std::size_t end = std::min(begin + chunk, numbers.size());
            parts.push_back(pool.submit([&numbers, begin, end] {
                return std::accumulate(numbers.begin() + static_cast<std::ptrdiff_t>(begin),
                                       numbers.begin() + static_cast<std::ptrdiff_t>(end), std::uint64_t{0});
            }));
        }
        std::uint64_t sum = 0;
        for (auto& p : parts) sum += p.get();
        const double ms = msSince(start);
        check(sum == expected, "分块求和的结果");
        std::cout << "  每个任务 " << chunk << " 个数（共 " << parts.size() << " 个任务）：" << ms << " ms\n";
    }
}

int main() {
    exceptionInTask();
    taskWaitingOnSamePool();
    blockingTasksStarveOthers();
    tasksTooSmall();
    std::cout << (failures == 0 ? "\n全部符合预期\n" : "\n有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
