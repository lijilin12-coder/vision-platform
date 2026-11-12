// 线程同步的基本工具：thread、mutex、lock_guard、atomic、condition_variable。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// ---------- 1. 互斥锁：同一时刻只允许一个线程进入 ----------

class SafeCounter {
public:
    void add() {
        std::lock_guard<std::mutex> lock(mutex_);  // 构造时加锁，离开作用域时自动解锁（RAII）
        ++value_;
    }
    int value() const {
        std::lock_guard<std::mutex> lock(mutex_);  // 读也要加锁
        return value_;
    }

private:
    mutable std::mutex mutex_;  // mutable：允许在 const 成员函数里加锁
    int value_ = 0;
};

// ---------- 2. 原子变量：单个数值的读写，不需要锁 ----------

std::atomic<int> atomicCounter{0};

// ---------- 3. 条件变量：一个线程等，另一个线程通知 ----------

// 只保存“最新一帧”的信箱。相机线程不断放入，检测线程取走。
class LatestFrame {
public:
    void put(int frameId) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_ = frameId;  // 旧的一帧如果还没被取走，直接覆盖
        }
        ready_.notify_one();  // 通知在等待的线程。先解锁再通知，被唤醒的线程可以立刻拿到锁
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    // 等到有新的一帧，或者信箱被关闭。关闭且没有剩余的帧时返回空。
    std::optional<int> take() {
        std::unique_lock<std::mutex> lock(mutex_);
        // wait 做三件事：解锁 → 睡眠 → 被唤醒后重新加锁并检查条件。
        // 条件不成立就继续睡。一定要带条件：线程可能被“虚假唤醒”。
        ready_.wait(lock, [this] { return frame_.has_value() || closed_; });
        if (!frame_.has_value()) return std::nullopt;
        const int id = *frame_;
        frame_.reset();
        return id;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<int> frame_;
    bool closed_ = false;
};

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

int main() {
    // 1. 互斥锁
    SafeCounter counter;
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&counter] {
                for (int i = 0; i < 100000; ++i) counter.add();
            });
        }
        for (auto& t : threads) t.join();  // 必须 join：等线程结束。忘记的话程序会直接终止
    }
    check(counter.value() == 400000, "互斥锁保护的计数器");

    // 2. 原子变量
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([] {
                for (int i = 0; i < 100000; ++i) ++atomicCounter;
            });
        }
        for (auto& t : threads) t.join();
    }
    check(atomicCounter.load() == 400000, "原子计数器");

    // 3. 条件变量
    LatestFrame mailbox;
    std::vector<int> received;
    std::thread consumer([&mailbox, &received] {
        while (const auto frame = mailbox.take()) received.push_back(*frame);
    });
    for (int id = 1; id <= 5; ++id) {
        mailbox.put(id);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 模拟相机的帧间隔
    }
    mailbox.close();
    consumer.join();  // join 之后才能安全地读 received

    check(!received.empty() && received.back() == 5, "最后一帧一定会被收到");
    bool increasing = true;
    for (std::size_t i = 1; i < received.size(); ++i) increasing = increasing && received[i] > received[i - 1];
    check(increasing, "帧号递增，没有重复");
    std::cout << "发送 5 帧，收到 " << received.size() << " 帧\n";

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
