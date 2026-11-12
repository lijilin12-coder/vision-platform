// 申请内存到底要花多少时间：小块和大块完全不同。
//
//   macOS / Linux：clang++ -std=c++17 -O2 alloc_cost.cpp -o alloc_cost && ./alloc_cost
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 alloc_cost.cpp && alloc_cost.exe
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Stats {
    double median = 0;
    double p99 = 0;
    double max = 0;
};

// 把 fn 运行 repeats 次，返回每次耗时（微秒）的中位数、99% 分位和最大值。
template <typename Fn>
Stats measureUs(int repeats, Fn&& fn) {
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(repeats));
    for (int i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        fn();
        times.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    return {times[times.size() / 2], times[times.size() * 99 / 100], times.back()};
}

volatile std::uint64_t sink = 0;

void print(const char* name, const Stats& s) {
    std::cout << "  " << name << "：中位数 " << s.median << " 微秒，99% 在 " << s.p99 << " 微秒以内，最慢 " << s.max
              << " 微秒\n";
}

int main() {
    // ---------- 小块：64 字节 ----------
    std::cout << "小块（64 字节），每次申请 1000 个再全部释放：\n";
    {
        std::vector<void*> blocks(1000);
        const Stats s = measureUs(2000, [&] {
            for (auto& b : blocks) b = std::malloc(64);
            sink = reinterpret_cast<std::uintptr_t>(blocks[500]);
            for (auto& b : blocks) std::free(b);
        });
        // 1000 次申请加释放一共 s.median 微秒，所以平均每次是 s.median 纳秒
        std::cout << "  平均每次申请加释放约 " << s.median << " 纳秒\n";
    }

    // ---------- 大块：一张 5MP 灰度图，约 5 MB ----------
    const std::size_t imageBytes = 2592u * 1944u;
    std::cout << "\n大块（一张 5MP 灰度图，约 5 MB），每次都要把整张图写一遍：\n";

    // 做法一：每次申请新的，用完释放
    const Stats fresh = measureUs(300, [&] {
        std::unique_ptr<std::uint8_t[]> buffer(new std::uint8_t[imageBytes]);  // 申请（不清零）
        std::memset(buffer.get(), 128, imageBytes);                            // 写入：模拟算子输出结果
        sink = buffer[imageBytes / 2];
    });
    print("每次申请新的缓冲区  ", fresh);

    // 做法二：用 std::vector 申请。vector 会先把每个字节清零，然后我们再写一遍：整张图被写了两次
    const Stats zeroed = measureUs(300, [&] {
        std::vector<std::uint8_t> buffer(imageBytes);    // 申请并清零
        std::memset(buffer.data(), 128, imageBytes);     // 再写入结果
        sink = buffer[imageBytes / 2];
    });
    print("每次新建 std::vector", zeroed);

    // 做法三：只申请一次，反复使用
    std::vector<std::uint8_t> reused(imageBytes);
    const Stats reuse = measureUs(300, [&] {
        std::memset(reused.data(), 128, imageBytes);
        sink = reused[imageBytes / 2];
    });
    print("复用同一个缓冲区    ", reuse);

    std::cout << "\n按中位数：复用比“每次申请新的”快 " << fresh.median / reuse.median << " 倍，比“每次新建 vector”快 "
              << zeroed.median / reuse.median << " 倍\n";
    std::cout << "按最慢的一次：" << fresh.max << " / " << zeroed.max << " / " << reuse.max << " 微秒\n";
    return EXIT_SUCCESS;
}
