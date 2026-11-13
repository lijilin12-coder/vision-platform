// 线程切换要花多少时间：让两个线程像打乒乓球一样轮流执行，测量每次交接的耗时。
//
// 每一次交接：当前线程通知对方、自己睡下，操作系统保存它的现场，恢复另一个线程的现场。
// 对比：同样次数的普通函数调用。
//
//   macOS / Linux：clang++ -std=c++17 -O2 -pthread context_switch.cpp -o context_switch && ./context_switch
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 context_switch.cpp && context_switch.exe
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

using Clock = std::chrono::steady_clock;

volatile long sink = 0;
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void plainCall(long i) { sink = i; }

int main() {
    const int rounds = 100000;

    // ---------- 普通函数调用 ----------
    auto start = Clock::now();
    for (int i = 0; i < rounds; ++i) plainCall(i);
    const double callNs = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / rounds;

    // ---------- 两个线程轮流执行 ----------
    std::mutex mutex;
    std::condition_variable changed;
    int turn = 0;   // 0：轮到 ping；1：轮到 pong
    long hits = 0;  // 一共击球多少次

    const auto player = [&](int me) {
        for (int i = 0; i < rounds; ++i) {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return turn == me; });  // 没轮到我：睡下，让出 CPU
            ++hits;
            turn = 1 - me;         // 轮到对方
            lock.unlock();
            changed.notify_one();  // 叫醒对方
        }
    };

    start = Clock::now();
    std::thread ping(player, 0);
    std::thread pong(player, 1);
    ping.join();
    pong.join();
    const double switchUs = std::chrono::duration<double, std::micro>(Clock::now() - start).count() / (2.0 * rounds);

    std::cout << "普通函数调用：每次约 " << callNs << " 纳秒\n";
    std::cout << "线程之间交接：每次约 " << switchUs << " 微秒（" << switchUs * 1000 << " 纳秒）\n";
    std::cout << "交接一次的时间，大约够调用 " << static_cast<long>(switchUs * 1000 / (callNs > 0 ? callNs : 1)) << " 次函数\n";

    return hits == 2L * rounds ? EXIT_SUCCESS : EXIT_FAILURE;
}
