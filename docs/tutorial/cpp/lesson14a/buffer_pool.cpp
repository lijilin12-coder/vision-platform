// 图像缓冲区池：缓冲区用完自动归还，下次直接复用。
//
// 要点：
//   - acquire() 返回一个 shared_ptr，它的“删除器”不是释放内存，而是把缓冲区还给池（RAII）。
//   - 池的状态放在 shared_ptr 里，缓冲区只持有它的 weak_ptr：
//     即使池先被销毁，还没归还的缓冲区也能安全地自己释放。
//   - 池里最多保留 maxIdle 个空闲缓冲区，多出来的直接释放：池的大小有上限。
//
//   macOS / Linux：clang++ -std=c++17 -O2 -pthread buffer_pool.cpp -o buffer_pool && ./buffer_pool
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 buffer_pool.cpp && buffer_pool.exe
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using Buffer = std::vector<std::uint8_t>;

class BufferPool {
public:
    explicit BufferPool(std::size_t maxIdle) : state_(std::make_shared<State>()) { state_->maxIdle = maxIdle; }

    // 取一个至少 size 字节的缓冲区。内容是上一次使用留下的，不保证清零。
    std::shared_ptr<Buffer> acquire(std::size_t size) {
        std::unique_ptr<Buffer> buffer;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            // 找一个容量够用的空闲缓冲区
            auto it = std::find_if(state_->idle.begin(), state_->idle.end(),
                                   [size](const std::unique_ptr<Buffer>& b) { return b->capacity() >= size; });
            if (it != state_->idle.end()) {
                buffer = std::move(*it);
                state_->idle.erase(it);
                ++state_->reused;
            } else {
                ++state_->created;
            }
        }
        if (!buffer) buffer = std::make_unique<Buffer>();
        buffer->resize(size);  // 容量够时不会重新申请内存

        // 自定义删除器：shared_ptr 的引用计数归零时调用它，而不是 delete。
        std::weak_ptr<State> weak = state_;
        return std::shared_ptr<Buffer>(buffer.release(), [weak](Buffer* raw) {
            std::unique_ptr<Buffer> returned(raw);
            if (const auto state = weak.lock()) {  // 池还在：还回去
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->idle.size() < state->maxIdle) state->idle.push_back(std::move(returned));
            }
            // 池已经不在了，或者池已经满了：returned 在这里析构，内存被释放
        });
    }

    std::size_t created() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->created;
    }
    std::size_t reused() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->reused;
    }
    std::size_t idleCount() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->idle.size();
    }

private:
    struct State {
        std::mutex mutex;
        std::vector<std::unique_ptr<Buffer>> idle;  // 空闲的缓冲区
        std::size_t maxIdle = 0;
        std::size_t created = 0;
        std::size_t reused = 0;
    };
    std::shared_ptr<State> state_;
};

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

using Clock = std::chrono::steady_clock;
volatile std::uint64_t sink = 0;

struct Stats {
    double median = 0;
    double max = 0;
};

template <typename Fn>
Stats measureUs(int repeats, Fn&& fn) {
    std::vector<double> times;
    for (int i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        fn();
        times.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    std::sort(times.begin(), times.end());
    return {times[times.size() / 2], times.back()};
}

int main() {
    const std::size_t imageBytes = 2592u * 1944u;

    // ---------- 1. 用完自动归还 ----------
    {
        BufferPool pool(4);
        const std::uint8_t* firstAddress = nullptr;
        {
            auto a = pool.acquire(imageBytes);
            firstAddress = a->data();
            auto alias = a;  // 复制 shared_ptr：两个持有者
            check(pool.idleCount() == 0, "还有人在用时，缓冲区不在池里");
        }  // 最后一个持有者离开作用域 → 删除器被调用 → 缓冲区回到池里
        check(pool.idleCount() == 1, "用完之后自动回到池里");

        auto b = pool.acquire(imageBytes);
        check(b->data() == firstAddress, "第二次取到的是同一块内存");
        check(pool.created() == 1 && pool.reused() == 1, "只真正申请过一次内存");

        auto smaller = pool.acquire(1000);  // b 还在使用中，池里没有空闲的
        check(pool.created() == 2, "没有空闲缓冲区时才新建");
    }

    // ---------- 2. 池的大小有上限 ----------
    {
        BufferPool pool(2);
        {
            auto a = pool.acquire(100), b = pool.acquire(100), c = pool.acquire(100), d = pool.acquire(100);
        }  // 四个同时归还，池里最多留两个
        check(pool.idleCount() == 2, "空闲缓冲区不超过上限，多出来的直接释放");
    }

    // ---------- 3. 池先销毁，缓冲区后归还 ----------
    {
        std::shared_ptr<Buffer> survivor;
        {
            BufferPool pool(2);
            survivor = pool.acquire(100);
        }  // 池在这里销毁，survivor 还活着
        (*survivor)[0] = 7;
        check((*survivor)[0] == 7, "池销毁之后，还没归还的缓冲区仍然可以安全使用");
    }  // survivor 在这里释放：池已经不在了，内存直接释放，不会访问已销毁的池

    // ---------- 4. 取到的缓冲区里是上一次留下的内容 ----------
    {
        BufferPool pool(2);
        {
            auto a = pool.acquire(16);
            std::memset(a->data(), 0xAB, 16);
        }
        auto b = pool.acquire(16);
        check((*b)[0] == 0xAB, "复用的缓冲区里是旧数据，不是 0");
    }

    // ---------- 5. 多个线程同时取和还 ----------
    {
        BufferPool pool(8);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&pool, t] {
                for (int i = 0; i < 2000; ++i) {
                    auto buffer = pool.acquire(4096);
                    std::memset(buffer->data(), t, 4096);  // 每个线程写自己拿到的那一块
                }
            });
        }
        for (auto& th : threads) th.join();
        check(pool.created() <= 4, "4 个线程轮流使用，最多只需要 4 个缓冲区");
        check(pool.created() + pool.reused() == 8000, "每次取用要么新建，要么复用");
        std::cout << "4 个线程共取用 8000 次：真正新建 " << pool.created() << " 个，复用 " << pool.reused() << " 次\n";
    }

    // ---------- 6. 模拟一次检测：四个节点各产生一张 5MP 的输出图像 ----------
    const auto fillLikeAnOperator = [](Buffer& out, int value) {
        std::memset(out.data(), value, out.size());
        sink = out[out.size() / 2];
    };

    const Stats withoutPool = measureUs(100, [&] {
        for (int node = 0; node < 4; ++node) {
            auto out = std::make_shared<Buffer>(imageBytes);  // 每个节点新建一个缓冲区（会先清零）
            fillLikeAnOperator(*out, node);
        }
    });

    BufferPool pool(4);
    const Stats withPool = measureUs(100, [&] {
        for (int node = 0; node < 4; ++node) {
            auto out = pool.acquire(imageBytes);
            fillLikeAnOperator(*out, node);
        }
    });

    std::cout << "一次检测（4 个节点，各输出一张 5MP 图像）：\n";
    std::cout << "  每次新建缓冲区：中位数 " << withoutPool.median << " 微秒，最慢 " << withoutPool.max << " 微秒\n";
    std::cout << "  使用缓冲区池：  中位数 " << withPool.median << " 微秒，最慢 " << withPool.max << " 微秒\n";

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
