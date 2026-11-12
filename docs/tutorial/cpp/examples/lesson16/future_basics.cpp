// promise 和 future：一个线程“承诺”一个结果，另一个线程“凭票”去取。
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

int slowSquare(int x) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // 模拟耗时的计算
    return x * x;
}

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

int main() {
    // ---------- 1. 最基本的用法 ----------
    {
        std::promise<int> promise;                      // “写”的一端
        std::future<int> future = promise.get_future(); // “读”的一端，只能取一次

        std::thread worker([p = std::move(promise)]() mutable {  // promise 不能复制，只能移动进线程
            p.set_value(slowSquare(7));                 // 兑现承诺
        });

        check(future.get() == 49, "get() 会一直等到结果出来");  // 阻塞，直到 set_value 被调用
        worker.join();
    }

    // ---------- 2. 异常也能跨线程传递 ----------
    {
        std::promise<int> promise;
        std::future<int> future = promise.get_future();
        std::thread worker([p = std::move(promise)]() mutable {
            try {
                throw std::runtime_error("模型推理失败");
            } catch (...) {
                p.set_exception(std::current_exception());  // 把异常存进去
            }
        });

        bool caught = false;
        try {
            (void)future.get();                         // 在这个线程里重新抛出
        } catch (const std::runtime_error& e) {
            caught = std::string(e.what()) == "模型推理失败";
        }
        check(caught, "工作线程里的异常，在 get() 的线程里被捕获");
        worker.join();
    }

    // ---------- 3. 承诺没有兑现 ----------
    {
        std::future<int> future;
        {
            std::promise<int> promise;
            future = promise.get_future();
        }  // promise 销毁了，但从来没有 set_value

        bool broken = false;
        try {
            (void)future.get();
        } catch (const std::future_error& e) {
            broken = e.code() == std::future_errc::broken_promise;
        }
        check(broken, "promise 没兑现就销毁：get() 抛出 broken_promise，而不是永远等下去");
    }

    // ---------- 4. 等待时可以设超时 ----------
    {
        std::promise<int> promise;
        std::future<int> future = promise.get_future();
        check(future.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout, "结果还没出来：超时");
        promise.set_value(1);
        check(future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready, "结果出来了：ready");
        check(future.get() == 1, "取值");
        check(!future.valid(), "get() 之后 future 失效，不能再取第二次");
    }

    // ---------- 5. shared_future：多个地方都要取同一个结果 ----------
    {
        std::promise<std::string> promise;
        std::shared_future<std::string> shared = promise.get_future().share();
        std::shared_future<std::string> copy = shared;  // 可以复制
        promise.set_value("OK");
        check(shared.get() == "OK" && copy.get() == "OK" && shared.get() == "OK", "shared_future 可以多次 get");
    }

    // ---------- 6. std::async：把“开线程 + promise”打包好了 ----------
    {
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::future<int>> futures;
        for (int i = 1; i <= 4; ++i) {
            // 一定要写 std::launch::async，否则标准允许它推迟到 get() 时才在当前线程里执行
            futures.push_back(std::async(std::launch::async, slowSquare, i));
        }
        int sum = 0;
        for (auto& f : futures) sum += f.get();
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        check(sum == 1 + 4 + 9 + 16, "四个任务的结果");
        std::cout << "4 个各需 30 ms 的任务并行执行，总共用了 " << ms << " ms\n";
        // 如果一个接一个地执行，至少需要 120 ms。这里只打印不做断言：耗时和机器的忙闲有关。
    }

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
