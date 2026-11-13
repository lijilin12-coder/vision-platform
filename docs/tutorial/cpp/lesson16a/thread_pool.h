// 一个最小的线程池：固定数量的工作线程，共用一个任务队列。
//
//   ThreadPool pool(4);
//   std::future<int> f = pool.submit([] { return 6 * 7; });
//   int answer = f.get();   // 42
//
// 教学用的实现，力求短而完整。和第 16 课推理服务的结构相同，只是把“一个工作线程”换成了多个，
// 把“一张图像”换成了“任意一段要执行的代码”。
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threadCount) {
        if (threadCount == 0) threadCount = 1;
        workers_.reserve(threadCount);
        for (std::size_t i = 0; i < threadCount; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    // 析构：不再接收新任务，把队列里剩下的任务做完，然后等所有工作线程结束。
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        available_.notify_all();
        for (auto& worker : workers_) worker.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    std::size_t size() const { return workers_.size(); }

    // 提交一个任务，立刻返回。任务的返回值（或它抛出的异常）以后通过 future 取。
    template <typename Fn>
    auto submit(Fn&& fn) -> std::future<std::invoke_result_t<std::decay_t<Fn>>> {
        using Result = std::invoke_result_t<std::decay_t<Fn>>;

        // packaged_task 把“一个函数”和“一个 promise”打包在一起：
        // 调用它时，会执行函数，并自动把返回值或异常放进 promise。
        // 用 shared_ptr 包一层，是因为 packaged_task 只能移动，而 std::function 要求里面的东西可以复制。
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
        std::future<Result> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) throw std::runtime_error("线程池正在停止，不能再提交任务");
            tasks_.push([task] { (*task)(); });
        }
        available_.notify_one();
        return result;
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                available_.wait(lock, [this] { return !tasks_.empty() || stopping_; });
                if (tasks_.empty()) return;  // 要停止了，并且队列已空
                task = std::move(tasks_.front());
                tasks_.pop();
            }        // ← 锁在这里释放：执行任务时不能拿着锁
            task();  // packaged_task 会接住任务里抛出的异常，所以这里不会把工作线程弄崩
        }
    }

    std::mutex mutex_;                         // 保护 tasks_ 和 stopping_
    std::condition_variable available_;        // 有任务了，或者要停止了
    std::queue<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;         // 最后声明：其他成员都准备好之后才启动线程
};
