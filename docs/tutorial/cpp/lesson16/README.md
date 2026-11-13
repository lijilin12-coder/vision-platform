# 第 16 课：`promise`、`future` 与生产者消费者——推理服务

> 预计用时：2.5 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

一台电脑上有 4 个工位，每个工位的方案里都有一个深度学习模型的推理节点。

1. 模型只加载一份，4 个工位线程怎么共用它？
2. 推理按批进行才快。怎么把来自不同工位的图像凑成一批？
3. 每个工位提交了图像之后，怎么拿回属于自己的那个结果？

答案是一个经典的结构：**生产者—消费者**，加上 `std::promise` 和 `std::future`。这是深度学习推理服务里最常见的写法。这一课把前面 15 课的内容几乎都用上了。

## 1. `promise` 和 `future`

上一课的条件变量可以让一个线程等另一个线程，但它传递的只是“有事了”这个信号。很多时候我们要的是：**一个线程算出一个结果，另一个线程拿到这个结果。**

`std::promise` 和 `std::future` 是一对，像取餐时的小票：

| | 是什么 | 谁拿着 |
|---|---|---|
| `std::promise<T>` | “写”的一端：我承诺以后会给出一个 `T` | 干活的线程 |
| `std::future<T>` | “读”的一端：凭它去取结果 | 等结果的线程 |

[`future_basics.cpp`](future_basics.cpp)：

```cpp
std::promise<int> promise;
std::future<int> future = promise.get_future();     // 从 promise 得到与它配对的 future

std::thread worker([p = std::move(promise)]() mutable {
    p.set_value(slowSquare(7));                     // 兑现承诺
});

int result = future.get();                          // 等到结果出来，然后取走
worker.join();
```

- `future.get()` 会**阻塞**，直到另一端调用了 `set_value`。
- `promise` 不能复制，只能移动（第 4 课）。`[p = std::move(promise)]` 是把它移动进 lambda 的写法。

### 异常也能传过来

```cpp
try {
    throw std::runtime_error("模型推理失败");
} catch (...) {
    p.set_exception(std::current_exception());      // 把异常存进去
}
```

```cpp
try {
    future.get();                                   // 在等结果的线程里重新抛出
} catch (const std::runtime_error& e) { ... }
```

工作线程里发生的错误，会在调用 `get()` 的地方原样抛出来。调用者处理错误的方式和单线程时一样。

### 承诺没有兑现

如果 `promise` 被销毁时既没有 `set_value` 也没有 `set_exception`，`future.get()` 会抛出 `std::future_error`（`broken_promise`），**而不是永远等下去**。这是一道安全网，但不要依赖它：正确的程序应该保证每个 `promise` 都被明确地兑现。

### 几个要点

| 要点 | 说明 |
|---|---|
| `get()` 只能调用一次 | 调用之后 `future` 就失效了。多个地方都要取同一个结果时，用 `future.share()` 得到 `std::shared_future`，它可以复制、可以多次 `get()` |
| 可以带超时地等 | `future.wait_for(std::chrono::milliseconds(100))` 返回 `ready` 或 `timeout`。产线上不能无限期地等，超过节拍就要按超时处理 |
| `std::async` | 把“开一个线程，用 promise 传回结果”打包成一行：`auto f = std::async(std::launch::async, slowSquare, 7);`。**一定要写 `std::launch::async`**，否则标准允许它不开线程，推迟到 `get()` 时才在当前线程里执行 |

示例里用 `std::async` 并行执行 4 个各需 30 ms 的任务，总共只用了约 31 ms。

## 2. 问题：多个工位，一个模型

为什么不让每个工位各加载一份模型、各推理各的？

| 原因 | 说明 |
|---|---|
| **内存和显存** | 一个模型几十到几百 MB，4 个工位各一份就是 4 倍 |
| **推理引擎通常不能被多个线程同时调用** | 尤其是用到 GPU、NPU 的时候 |
| **成批推理快得多** | 推理一次有固定的开销。一批处理 8 张图，比分 8 次各处理 1 张快好几倍 |

所以合理的结构是：**一个专门的推理线程独占模型**，各个工位把图像交给它，它凑成批，推理完再把结果分发回去。

```
工位 1 ─┐                                    ┌─► 工位 1 的结果
工位 2 ─┼─► commit(图像) ─► [任务队列] ─► 推理线程 ─┼─► 工位 2 的结果
工位 3 ─┤   立刻返回 future              一次取一批  ├─► 工位 3 的结果
工位 4 ─┘                               一起推理    └─► 工位 4 的结果
  生产者                                  消费者
```

每个任务里带着一个 `promise`。推理线程算出结果后调用 `set_value`，对应的工位就从自己手里的 `future` 拿到了结果。**不需要任何额外的机制来对应“这个结果是谁的”：任务和结果通过 promise/future 这一对天然地连在一起。**

## 3. 对外的接口

[`inference_worker.cpp`](inference_worker.cpp)。使用者能看到的只有这些：

```cpp
class Infer {
public:
    virtual ~Infer() = default;
    virtual std::future<Prediction> commit(Tensor input) = 0;
};

std::shared_ptr<Infer> createInfer(const std::string& modelPath, int maxBatchSize, std::size_t queueCapacity = 64);
```

用法：

```cpp
const auto infer = createInfer("scratch-detector.onnx", 8);
if (!infer) { /* 模型加载失败 */ }

std::future<Prediction> f = infer->commit(image);   // 立刻返回，不等推理
// ……可以继续做别的，比如传统算法的节点……
Prediction p = f.get();                             // 需要结果时再等
```

这里用了前面学过的三样东西：

- **接口加工厂函数**（第 8 课）：线程、队列、锁、模型全部藏在实现里。使用者不可能误用它们。
- **RAII**（第 4 课）：`infer` 销毁时自动停止线程、释放模型。没有 `start()`、`stop()` 需要记得调用。
- **创建失败时返回空**（第 6 课）：拿到手的 `Infer` 一定是可用的。

## 4. 实现

### 任务和共享状态

```cpp
struct Job {
    Tensor input;
    std::promise<Prediction> promise;
};

std::mutex mutex_;                   // 保护下面三个成员
std::condition_variable notEmpty_;   // 队列里有任务了，或者要停止了
std::condition_variable notFull_;    // 队列有空位了，或者要停止了
std::queue<Job> jobs_;
bool running_ = false;

std::thread worker_;
```

锁和它保护的数据放在一起，全部私有（上一课的第一条规则）。

### 生产者：`commit`

```cpp
std::future<Prediction> commit(Tensor input) override {
    Job job;
    job.input = std::move(input);
    std::future<Prediction> result = job.promise.get_future();   // 先取出 future，再把 job 交出去

    {
        std::unique_lock<std::mutex> lock(mutex_);
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
```

`commit` 只做三件事：打包任务、放进队列、通知消费者。**它不等推理**，所以很快就返回。

`notFull_.wait(...)` 是**背压**：队列满了，生产者就停下来等。如果不设上限，生产得比消费快时队列会无限增长，这正是第 13 课说的“没有泄漏但内存一直涨”。

### 消费者：工作线程

```cpp
void workerLoop(std::string modelPath, std::promise<bool> started) {
    std::unique_ptr<FakeModel> model = FakeModel::load(modelPath);     // ① 在工作线程里加载模型
    if (!model) {
        { std::lock_guard<std::mutex> lock(mutex_); running_ = false; }
        started.set_value(false);
        return;
    }
    started.set_value(true);                                           // ② 报告加载成功

    std::vector<Job> batch;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            notEmpty_.wait(lock, [this] { return !jobs_.empty() || !running_; });
            if (jobs_.empty()) break;                                  // ③ 要停止了，并且队列已空

            while (!jobs_.empty() && static_cast<int>(batch.size()) < maxBatchSize_) {   // ④ 取一批
                batch.push_back(std::move(jobs_.front()));
                jobs_.pop();
            }
        }                                                              // ⑤ 锁在这里释放
        notFull_.notify_all();

        runBatch(*model, batch);                                       // ⑥ 在锁外推理
        batch.clear();
    }
}
```

逐条说明：

**① 模型在工作线程里加载。** 加载、推理、释放都在同一个线程里完成。很多推理引擎要求这样，尤其是用到 GPU 或 NPU 时，它们的上下文和创建它的线程绑定。

**② 用一个 `promise<bool>` 报告启动结果。** 模型在另一个线程里加载，创建者怎么知道成功没有？这里又用了一次 promise/future：

```cpp
bool startup(const std::string& modelPath) {
    std::promise<bool> started;
    std::future<bool> loaded = started.get_future();
    running_ = true;
    worker_ = std::thread(&InferImpl::workerLoop, this, modelPath, std::move(started));
    return loaded.get();              // 等到加载完成
}
```

`createInfer` 调用 `startup`，失败就返回空指针。所以调用者拿到的 `Infer` 一定已经加载好了模型。

**③ 退出的条件是“要停止了，并且队列已空”。** 已经提交的任务会被处理完，不会丢。

**④ 一次取一批。** 队列里有几个就取几个，最多 `maxBatchSize_` 个。这种“有多少取多少”的策略很简单：负载低时每批只有 1 个，延迟最小；负载高时自然凑成大批，吞吐最高。

**⑤⑥ 推理一定在锁外做。** 这是整个设计里最重要的一点。推理要几毫秒到几十毫秒，如果拿着锁做，这段时间里所有工位的 `commit` 都会被卡住。

### 兑现每一个承诺

```cpp
static void runBatch(FakeModel& model, std::vector<Job>& batch) {
    // ……输入为空的任务：单独 set_exception，不影响同一批的其他任务……
    try {
        const std::vector<float> scores = model.inferBatch(inputs);
        for (std::size_t i = 0; i < owners.size(); ++i) {
            batch[owners[i]].promise.set_value(Prediction{scores[i], ...});
        }
    } catch (...) {
        for (const std::size_t owner : owners) batch[owner].promise.set_exception(std::current_exception());
    }
}
```

规则是：**每个 `promise` 必须恰好被兑现一次，要么是值，要么是异常。**

- 一次都不兑现：对应的工位会卡在 `get()` 上，或者收到莫名其妙的 `broken_promise`。
- 兑现两次：第二次会抛异常。

所以推理代码要包在 `try` 里：模型抛了异常，也要保证这一批的每个任务都收到通知。

### 停止

```cpp
~InferImpl() override {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    notEmpty_.notify_all();
    notFull_.notify_all();
    if (worker_.joinable()) worker_.join();
}
```

三步：在锁里设置停止标志 → 叫醒所有在等的线程 → 等工作线程结束。

`running_ = false` 必须在锁里设置。如果不加锁，可能出现这样的情况：工作线程刚检查完条件（还是 `true`），正要去睡，这时主线程设置了标志并发出通知；工作线程随后睡下，却再也没有人叫醒它。

## 5. 运行结果

```
64 个任务，每批最多 1 个：307 ms（最大批 1）
64 个任务，每批最多 8 个：74 ms（最大批 8）
```

4 个线程各提交 16 个任务。同样的 64 个任务，成批推理快了 4 倍。示例里的模型是假的（用睡眠模拟耗时），真实的推理引擎在 GPU 和 NPU 上有同样的规律。

示例还验证了：

| 情况 | 结果 |
|---|---|
| 模型文件不存在 | `createInfer` 返回空 |
| 4 个线程同时提交 | 每个任务都拿到了属于自己的结果 |
| 一批里有一个任务的输入不合法 | 只有那个任务收到异常，其他任务照常 |
| 提交 20 个任务后立刻销毁推理服务 | 20 个任务全部完成，没有丢失，也没有卡死 |
| 队列容量只有 2，提交 30 个任务 | 生产者被背压挡住，最终全部完成 |

整个程序在 ThreadSanitizer 和 AddressSanitizer 下运行都没有任何报告。

## 6. 常见错误

| 错误 | 后果 | 正确做法 |
|---|---|---|
| 拿着锁推理 | 所有生产者被卡住，多线程比单线程还慢 | 在锁里只做“取任务”，推理在锁外 |
| 条件变量的 `wait` 不带条件 | 虚假唤醒后从空队列里取任务 | 永远带条件 |
| 停止标志不在锁里设置 | 偶尔卡死在析构里 | 设置标志时加锁，然后 `notify_all` |
| 忘记 `notify` | 线程永远睡着 | 每次改变“别人在等的条件”之后都通知 |
| 有的路径忘了兑现 `promise` | 调用者卡住或收到 `broken_promise` | 每条路径（成功、失败、停止）都兑现 |
| 队列没有上限 | 消费跟不上时内存无限增长 | 有界队列加背压 |
| lambda 按引用捕获了局部变量再交给线程 | 线程运行时变量已经不存在 | 交给线程的东西按值捕获或移动进去 |
| 对同一个 `future` 调用两次 `get()` | 抛异常 | 需要多次取时用 `shared_future` |
| 析构时不 `join` | 程序终止 | 在析构函数里 `join` |

## 7. 可以改进的地方

这个示例是最小的完整版本。实际使用时可能还需要：

| 改进 | 做法 |
|---|---|
| **等一小会儿凑批** | 取到第一个任务后，再等几毫秒看有没有更多任务。延迟略增，批更大。用 `wait_for` 实现 |
| **超时** | 调用者用 `future.wait_for(节拍)`；超过节拍还没结果，这次检测按超时处理 |
| **优先级** | 多个工位争用同一个模型时，重要的工位先处理（设计文档里的 H-10） |
| **停止时放弃排队的任务** | 需要尽快停止时，给队列里剩下的任务 `set_exception`，而不是把它们处理完 |
| **多个模型** | 每个模型一个推理线程；或者一个线程轮流服务几个模型 |

## 8. 和本项目的关系

项目里的推理后端接口 `InferBackend` 还没有实现（需求 EXE-05）。实现时会用这一课的结构：

- **推理节点**（一个算子）调用 `commit` 提交图像，然后 `get()` 等结果。算子本身仍然是同步的，执行引擎不需要改。
- **多个工位**的推理节点共用同一个推理服务，自然形成批处理。
- **热切换**（POC-9）时，新上线包里的模型在后台加载和预热，正好对应这里“在工作线程里加载，用 promise 报告结果”的做法。
- **节拍**超时用 `wait_for` 判断，对应事件“检测已超时”。

## 9. 动手运行

**macOS / Linux**：

```bash
clang++ -std=c++17 -Wall -Wextra -g -pthread future_basics.cpp -o future_basics && ./future_basics
clang++ -std=c++17 -Wall -Wextra -g -pthread inference_worker.cpp -o inference_worker && ./inference_worker
```

用线程检查工具再运行一遍：

```bash
clang++ -std=c++17 -g -pthread -fsanitize=thread inference_worker.cpp -o inference_tsan && ./inference_tsan
```

**Windows**：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 future_basics.cpp && future_basics.exe
cl /std:c++17 /W4 /EHsc /utf-8 inference_worker.cpp && inference_worker.exe
```

Windows 上睡眠的最小时间比较长（约 15 ms），所以两种批大小的绝对耗时都会比上面的数字大，但成批更快这个结论不变。

## 10. 练习

1. **读懂每一处**：对照第 4 节，在 `inference_worker.cpp` 里找到 ①～⑥ 对应的代码。然后回答：`commit` 里为什么要先 `get_future()`，再把 `job` 放进队列？反过来行不行？
2. **亲手制造一个错误**：把 `workerLoop` 里的 `runBatch(*model, batch);` 移到加锁的那个 `{ }` 里面，重新运行。耗时有什么变化？为什么？（改回去。）
3. **等一小会儿凑批**：修改 `workerLoop`：取到任务后，如果还没凑满一批，就用 `notEmpty_.wait_for` 最多再等 2 毫秒。对比修改前后“每批最多 8 个”的耗时和平均批大小。
4. **加上超时**：在 `runStations` 里，把 `futures[j].get()` 改成先 `wait_for(100 毫秒)`，超时就计数。然后把 `FakeModel::inferBatch` 的睡眠时间调大，观察超时的数量。
5. **停止时放弃排队的任务**：给 `Infer` 增加 `void shutdownNow()`，调用后队列里还没开始的任务全部收到异常“已取消”。注意：每个 `promise` 仍然必须恰好兑现一次。
6. **设计题**：两个工位，一个检测关键尺寸（必须在节拍内完成），一个只做统计（慢一点没关系）。它们共用一个推理服务。你会怎么改 `commit` 的接口和队列的结构？

## 11. 自测题

1. `promise` 和 `future` 各由谁持有？各自能做什么？
2. 工作线程里抛出的异常，怎么让等结果的线程知道？
3. 为什么让一个专门的线程独占模型，而不是每个工位各推理各的？
4. `commit` 为什么能立刻返回？调用者什么时候才会等待？
5. 为什么推理必须在锁外进行？
6. 什么是背压？没有它会怎样？
7. 模型是在工作线程里加载的，`createInfer` 怎么知道加载成功了没有？
8. 析构函数里为什么要在加锁的情况下设置 `running_ = false`？
9. “每个 promise 必须恰好兑现一次”：少了会怎样，多了会怎样？

## 主线到这里结束

回头看一下前面 17 课的路线：

| 阶段 | 课 | 学到了什么 |
|---|---|---|
| 换一种写法 | 0～3 | 工程代码和竞赛代码的区别；类、头文件、CMake；拷贝与移动 |
| 写出可靠的类 | 4～7 | 资源自动管理；让非法状态创建不出来；明确地报告错误；用类型表达“几种之一” |
| 组织大一点的程序 | 8～11 | 接口与依赖倒置；算法与 lambda；模板与宏；测试 |
| 工程能力 | 12～14 | 调试；内存分析；性能分析 |
| 多线程 | 15～16 | 线程同步；promise/future；生产者消费者 |

最后这一课不到 300 行的代码里，几乎每一课的内容都出现了：接口和工厂函数、RAII、`unique_ptr` 和 `shared_ptr`、移动语义、lambda 的捕获规则、异常、`const`、互斥锁、条件变量。它们不是一堆孤立的语法，而是合在一起才能写出一个**用不错、不泄漏、不卡死**的组件。

接下来是延伸课 [线程池](../lesson16a/README.md)：把这一课的“一个工作线程”推广到多个。然后是最后一课：[第 17 课：日志与崩溃转储](../lesson17/README.md)，讲程序在产线上出了问题、又没法重现时怎么排查。

之后最好的学习方式是动手：按 [如何新增一个算子](../../../dev/how-to-add-operator.md) 写一个算子，给它写测试，然后读 `core/` 下的代码。读不懂的地方，回到对应的那一课。
