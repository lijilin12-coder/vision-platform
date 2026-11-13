// 用 promise / future 实现“生产者—消费者”的推理服务。
//
//   生产者（多个工位线程）：commit(图像) → 立刻拿到一个 future，不等结果
//   消费者（一个推理线程）：从队列里一次取出若干任务，组成一批，一起推理，再逐个兑现 promise
//
// 这是深度学习推理服务里最常见的结构：模型只有一份，推理按批进行，调用者各等各的结果。
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using Tensor = std::vector<float>;  // 输入：一张预处理好的图像

struct Prediction {
    float score = 0;     // 异常分数
    int batchSize = 0;   // 这次推理和多少个任务一起成批（只为了演示）
};

// ---------- 一个假的模型：用 sleep 模拟推理耗时 ----------
// 真实的推理引擎有同样的特点：每次调用有固定开销，所以一批处理 8 张，比分 8 次各处理 1 张快得多。

class FakeModel {
public:
    static std::unique_ptr<FakeModel> load(const std::string& path) {
        if (path.empty() || path.find("missing") != std::string::npos) return nullptr;  // 模拟“文件不存在”
        std::this_thread::sleep_for(std::chrono::milliseconds(20));                     // 模拟加载耗时
        return std::unique_ptr<FakeModel>(new FakeModel());
    }

    std::vector<float> inferBatch(const std::vector<Tensor>& batch) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));                       // 每次调用的固定开销
        std::this_thread::sleep_for(std::chrono::microseconds(500) * batch.size());      // 每张图的开销
        std::vector<float> scores;
        for (const auto& input : batch) scores.push_back(std::accumulate(input.begin(), input.end(), 0.0f));
        return scores;
    }

private:
    FakeModel() = default;
};

// ---------- 对外的接口：使用者只看到这些 ----------

class Infer {
public:
    virtual ~Infer() = default;
    // 提交一个任务，立刻返回。结果以后通过 future 取。
    virtual std::future<Prediction> commit(Tensor input) = 0;
};

// 创建推理服务。模型加载失败时返回空指针。
std::shared_ptr<Infer> createInfer(const std::string& modelPath, int maxBatchSize, std::size_t queueCapacity = 64);

// ---------- 实现：全部藏在这下面 ----------

namespace {

class InferImpl final : public Infer {
public:
    InferImpl(int maxBatchSize, std::size_t queueCapacity)
        : maxBatchSize_(maxBatchSize), queueCapacity_(queueCapacity) {}

    // 析构：通知工作线程停止，并等它结束（RAII）。已经在队列里的任务会被处理完。
    ~InferImpl() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // 启动工作线程，并等它报告模型是否加载成功。
    bool startup(const std::string& modelPath) {
        std::promise<bool> started;                       // 工作线程用它把“加载结果”告诉我们
        std::future<bool> loaded = started.get_future();
        running_ = true;
        worker_ = std::thread(&InferImpl::workerLoop, this, modelPath, std::move(started));
        return loaded.get();                              // 等到加载完成（成功或失败）
    }

    std::future<Prediction> commit(Tensor input) override {
        Job job;
        job.input = std::move(input);
        std::future<Prediction> result = job.promise.get_future();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 队列满了就等：这叫“背压”。生产得比消费快时，让生产者慢下来，而不是让内存无限增长。
            notFull_.wait(lock, [this] { return jobs_.size() < queueCapacity_ || !running_; });
            if (!running_) {
                job.promise.set_exception(std::make_exception_ptr(std::runtime_error("推理服务已停止")));
                return result;
            }
            jobs_.push(std::move(job));
        }
        notEmpty_.notify_one();
        return result;
    }

private:
    struct Job {
        Tensor input;
        std::promise<Prediction> promise;
    };

    void workerLoop(std::string modelPath, std::promise<bool> started) {
        // 模型在工作线程里加载：加载、推理、释放都在同一个线程。
        // 很多推理引擎（尤其用到 GPU、NPU 时）要求如此。
        std::unique_ptr<FakeModel> model = FakeModel::load(modelPath);
        if (!model) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_ = false;
            }
            started.set_value(false);
            return;
        }
        started.set_value(true);

        std::vector<Job> batch;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                notEmpty_.wait(lock, [this] { return !jobs_.empty() || !running_; });
                if (jobs_.empty()) break;  // 走到这里说明 running_ 为 false 且队列已空：可以退出了

                // 一次最多取 maxBatchSize_ 个，组成一批
                while (!jobs_.empty() && static_cast<int>(batch.size()) < maxBatchSize_) {
                    batch.push_back(std::move(jobs_.front()));
                    jobs_.pop();
                }
            }                        // ← 锁在这里释放。推理很慢，绝对不能拿着锁做
            notFull_.notify_all();   // 队列腾出了位置，叫醒被背压挡住的生产者

            runBatch(*model, batch);
            batch.clear();
        }
    }

    // 推理一批，并逐个兑现 promise。每个 promise 必须恰好被兑现一次：要么值，要么异常。
    static void runBatch(FakeModel& model, std::vector<Job>& batch) {
        std::vector<Tensor> inputs;
        std::vector<std::size_t> owners;  // inputs[i] 属于 batch[owners[i]]
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (batch[i].input.empty()) {
                // 单个任务的输入不合法：只让这一个任务失败，不影响同一批里的其他任务
                batch[i].promise.set_exception(std::make_exception_ptr(std::invalid_argument("输入为空")));
                continue;
            }
            inputs.push_back(std::move(batch[i].input));
            owners.push_back(i);
        }
        if (inputs.empty()) return;

        try {
            const std::vector<float> scores = model.inferBatch(inputs);
            for (std::size_t i = 0; i < owners.size(); ++i) {
                batch[owners[i]].promise.set_value(Prediction{scores[i], static_cast<int>(inputs.size())});
            }
        } catch (...) {
            // 整批推理失败：这一批里每个还没兑现的任务都收到同一个异常
            for (const std::size_t owner : owners) batch[owner].promise.set_exception(std::current_exception());
        }
    }

    const int maxBatchSize_;
    const std::size_t queueCapacity_;

    std::mutex mutex_;                   // 保护下面三个成员
    std::condition_variable notEmpty_;   // 队列里有任务了，或者要停止了
    std::condition_variable notFull_;    // 队列有空位了，或者要停止了
    std::queue<Job> jobs_;
    bool running_ = false;

    std::thread worker_;
};

}  // namespace

std::shared_ptr<Infer> createInfer(const std::string& modelPath, int maxBatchSize, std::size_t queueCapacity) {
    auto infer = std::make_shared<InferImpl>(maxBatchSize, queueCapacity);
    if (!infer->startup(modelPath)) return nullptr;  // 加载失败：infer 在这里析构，工作线程被 join
    return infer;
}

// ---------- 演示 ----------

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

// 4 个“工位”线程各提交 16 个任务，返回总耗时（毫秒）和观察到的最大批大小。
std::pair<double, int> runStations(int maxBatchSize) {
    const auto infer = createInfer("scratch-detector.onnx", maxBatchSize);
    if (!infer) return {-1.0, 0};

    const int stations = 4;
    const int jobsPerStation = 16;
    std::vector<int> wrong(stations, 0);
    std::vector<int> largestBatch(stations, 0);

    const auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int s = 0; s < stations; ++s) {
        threads.emplace_back([&, s] {
            std::vector<std::future<Prediction>> futures;
            for (int j = 0; j < jobsPerStation; ++j) {
                futures.push_back(infer->commit(Tensor{static_cast<float>(s), static_cast<float>(j)}));  // 不等结果
            }
            for (int j = 0; j < jobsPerStation; ++j) {
                const Prediction p = futures[static_cast<std::size_t>(j)].get();  // 各等各的结果
                if (p.score != static_cast<float>(s + j)) ++wrong[static_cast<std::size_t>(s)];
                if (p.batchSize > largestBatch[static_cast<std::size_t>(s)]) largestBatch[static_cast<std::size_t>(s)] = p.batchSize;
            }
        });
    }
    for (auto& t : threads) t.join();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    check(std::accumulate(wrong.begin(), wrong.end(), 0) == 0, "每个任务都拿到了属于自己的结果");
    int largest = 0;
    for (const int b : largestBatch) largest = b > largest ? b : largest;
    return {ms, largest};
}

int main() {
    // 1. 模型加载失败：创建时就知道
    check(createInfer("missing.onnx", 8) == nullptr, "模型不存在：createInfer 返回空");

    // 2. 4 个工位共用一个推理线程
    const auto [oneByOne, batch1] = runStations(1);
    const auto [batched, batch8] = runStations(8);
    std::cout << "64 个任务，每批最多 1 个：" << oneByOne << " ms（最大批 " << batch1 << "）\n";
    std::cout << "64 个任务，每批最多 8 个：" << batched << " ms（最大批 " << batch8 << "）\n";
    check(batch1 == 1, "每批最多 1 个时，批大小恒为 1");
    check(batch8 > 1 && batch8 <= 8, "每批最多 8 个时，确实组成了批");
    check(batched < oneByOne, "成批推理更快");

    // 3. 单个任务出错，不影响别的任务
    {
        const auto infer = createInfer("scratch-detector.onnx", 8);
        auto good1 = infer->commit(Tensor{1, 2, 3});
        auto bad = infer->commit(Tensor{});  // 空输入
        auto good2 = infer->commit(Tensor{4});
        check(good1.get().score == 6 && good2.get().score == 4, "正常的任务照常得到结果");
        bool threw = false;
        try {
            (void)bad.get();
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "出错的任务在 get() 时收到异常");
    }

    // 4. 销毁推理服务：已提交的任务会被处理完，不会丢，也不会卡死
    {
        std::vector<std::future<Prediction>> pending;
        {
            const auto infer = createInfer("scratch-detector.onnx", 4);
            for (int i = 0; i < 20; ++i) pending.push_back(infer->commit(Tensor{static_cast<float>(i)}));
        }  // infer 在这里析构：等工作线程把队列处理完
        int done = 0;
        for (auto& f : pending) {
            if (f.get().score >= 0) ++done;
        }
        check(done == 20, "析构前提交的 20 个任务全部完成");
    }

    // 5. 背压：队列容量只有 2，提交 30 个任务。生产者会被挡住，但最终全部完成
    {
        const auto infer = createInfer("scratch-detector.onnx", 2, 2);
        std::vector<std::future<Prediction>> futures;
        for (int i = 0; i < 30; ++i) futures.push_back(infer->commit(Tensor{1.0f}));
        int done = 0;
        for (auto& f : futures) done += f.get().score == 1.0f ? 1 : 0;
        check(done == 30, "队列很小也能全部完成");
    }

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
