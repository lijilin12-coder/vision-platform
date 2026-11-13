# 第 15 课：并发基础——线程、互斥锁、条件变量

> 预计用时：2 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起
>
> 如果你还不清楚线程在计算机里是什么样子、它和进程有什么区别，请先读 [第 15 课（预备）：线程和进程到底是什么](../lesson15a/README.md)。

## 这一课要回答的问题

检测平台天然是多线程的：相机在一个线程里采图，检测在另一个线程里运行，界面还要同时响应操作；一台电脑上有多个工位同时运行；新的上线包要在后台加载，不能打断正在进行的检测。

1. 怎么创建线程？
2. 两个线程同时访问同一份数据会出什么问题，怎么解决？
3. 一个线程怎么等另一个线程把事情做完？

这一课讲基础工具，下一课用它们搭建推理服务。

## 1. 线程

```cpp
#include <thread>

void work() { ... }

std::thread t(work);      // 创建线程，立刻开始执行 work()
// ……主线程继续做别的……
t.join();                 // 等 t 结束
```

也可以传 lambda 和参数：

```cpp
std::thread t([&counter] { for (int i = 0; i < 100000; ++i) counter.add(); });
```

**每个线程都必须 `join()`**（或者 `detach()`，但基本不要用）。一个还能 `join` 的 `std::thread` 对象被销毁时，程序会直接终止。第 4 课的 RAII 在这里很有用：把线程放进一个类，在析构函数里 `join`。下一课的推理服务就是这样做的。

传给线程的 lambda 会在另一个线程里**之后**才执行，所以第 9 课的捕获规则在这里是硬性要求：**按引用捕获的东西，必须保证活得比线程久。**

## 2. 数据竞争

[`race.cpp`](race.cpp)：两个线程各把同一个计数器加 100 万次。

```cpp
int counter = 0;

void addMany() {
    for (int i = 0; i < 1000000; ++i) ++counter;
}
```

连续运行三次：

```
期望 2000000，实际 1076691
期望 2000000，实际 1237351
期望 2000000，实际 1159495
```

结果不对，而且**每次都不一样**。

### 原因

（预备课第 8 节从寄存器和内存的角度画了这个过程。）

`++counter` 看起来是一步，实际是三步：

```
1. 把 counter 的值读出来
2. 加一
3. 把结果写回 counter
```

两个线程的这三步可能交错执行：

```
线程 A：读到 100
线程 B：读到 100
线程 A：算出 101，写回
线程 B：算出 101，写回        ← A 的那次加一被覆盖了
```

两个线程各加了一次，结果只增加了 1。

**两个线程同时访问同一份数据，并且至少有一个在写**，这叫**数据竞争**。在 C++ 里它是未定义行为，后果不只是“结果不对”，程序做出任何事情都是合法的。

数据竞争是最难查的一类 bug：

- **不是每次都出现。** 取决于两个线程恰好怎么交错，调试时很可能一切正常。
- **加了打印或断点就消失。** 因为时序变了。
- **换台机器、换个负载就表现不同。**

所以不能靠运行几次没出错来判断代码是对的。

### 用工具发现：ThreadSanitizer

```bash
clang++ -std=c++17 -g -fsanitize=thread race.cpp -o race && ./race
```

```
WARNING: ThreadSanitizer: data race
  Write of size 4 at 0x... by thread T2:
    #0 addMany() .../race.cpp:10
  Previous write of size 4 at 0x... by thread T1:
    #0 addMany() .../race.cpp:10
```

它指出了是哪两个线程、在哪一行、访问了同一个变量。ThreadSanitizer（TSan）在 macOS 和 Linux 上可用，Windows 的 MSVC 不支持。本项目的自动构建在 Linux 上运行，多线程代码在那里用 TSan 检查。

TSan 不能和 ASan 同时开启，要分两次编译。

## 3. 互斥锁

解决办法是保证**同一时刻只有一个线程**能执行那段代码（[`sync.cpp`](sync.cpp)）：

```cpp
class SafeCounter {
public:
    void add() {
        std::lock_guard<std::mutex> lock(mutex_);   // 加锁
        ++value_;
    }                                               // lock 析构：自动解锁
    int value() const {
        std::lock_guard<std::mutex> lock(mutex_);   // 读也要加锁
        return value_;
    }
private:
    mutable std::mutex mutex_;
    int value_ = 0;
};
```

`std::mutex` 是一把锁。一个线程加了锁，其他线程再来加锁就会停下等待，直到它解锁。

**永远不要手动调用 `mutex.lock()` 和 `mutex.unlock()`。** 用 `std::lock_guard`：构造时加锁，析构时解锁。无论函数从哪里返回、有没有抛异常，锁都一定会释放。这是第 4 课 RAII 最典型的应用。

`mutable` 让 `const` 成员函数也能加锁。加锁并没有改变对象在逻辑上的值。

### 使用锁的规则

| 规则 | 原因 |
|---|---|
| **把锁和它保护的数据放在同一个类里，数据设为 `private`** | 外面的代码只能通过成员函数访问，不可能忘记加锁。这是第 1 课封装的直接应用 |
| **读也要加锁** | 一个线程在读、另一个在写，同样是数据竞争 |
| **加锁的范围尽量小** | 锁住的时间里，其他线程都在等。**绝对不要拿着锁做耗时的事**（推理、读写文件、等待网络） |
| **不要在持有锁的时候调用不了解的代码** | 它可能又去加同一把锁，或者加另一把锁，造成死锁 |
| **需要同时加多把锁时，所有线程按同一个顺序加** | 否则 A 拿着锁 1 等锁 2，B 拿着锁 2 等锁 1，谁也动不了，这叫死锁 |

### 项目里的例子

[`in_memory_event_bus.cpp`](../../../../adapters/eventbus/src/in_memory_event_bus.cpp)：

```cpp
void InMemoryEventBus::publish(const EventPtr& event) {
    std::vector<Handler> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // ……把要通知的处理器复制到 targets……
    }                                    // ← 锁在这里释放
    for (auto& h : targets) h(event);    // 在锁外调用处理器
}
```

它只在“复制处理器列表”时持有锁，真正调用处理器时已经解锁了。这同时遵守了上面的第三、四条规则：处理器可能很慢，也可能在里面再次发布事件。如果拿着锁调用，后一种情况会让程序卡死。

[`execution_service.cpp`](../../../../core/contexts/execution/application/src/execution_service.cpp) 里的执行计划缓存也是同样的模式：查缓存时加锁，编译执行计划（比较慢）在锁外进行，写回缓存时再加锁。

## 4. 原子变量

只是一个整数或布尔值的简单读写时，可以用 `std::atomic`，不需要锁：

```cpp
std::atomic<int> atomicCounter{0};
++atomicCounter;                 // 这一步是不可分割的
```

`std::atomic<bool>` 常用来做“停止标志”：一个线程设置，另一个线程定期检查。

原子变量只能保证**单个操作**不被打断。“先检查再修改”这样的两步操作，或者要同时保持几个变量一致时，仍然需要锁。

## 5. 条件变量：等到某件事发生

检测线程要等相机送来新的一帧。最笨的办法是不停地检查：

```cpp
while (!hasFrame()) { }          // 白白占满一个 CPU 核心
```

**条件变量**让线程睡着等，有事了再被叫醒。示例里是一个只保存“最新一帧”的信箱：

```cpp
class LatestFrame {
public:
    void put(int frameId) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_ = frameId;
        }
        ready_.notify_one();                 // 叫醒一个在等的线程
    }

    std::optional<int> take() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return frame_.has_value() || closed_; });   // 等到条件成立
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
```

### `wait` 做了什么

```cpp
ready_.wait(lock, 条件);
```

等价于：

```cpp
while (!条件()) {
    // 1. 解锁（否则别的线程没法 put）
    // 2. 睡眠，直到被 notify
    // 3. 醒来，重新加锁
}
```

三个要点：

- **必须配合一把锁。** 条件（`frame_` 有没有值）是共享数据，检查它时必须持有锁。
- **用 `std::unique_lock`，不是 `lock_guard`。** 因为 `wait` 需要在中途解锁和重新加锁，`lock_guard` 做不到。
- **一定要带条件。** 线程可能在没有人通知的情况下醒来（叫“虚假唤醒”），也可能醒来时数据已经被别的线程取走了。带条件的 `wait` 会在醒来后重新检查，不成立就继续睡。

### 一定要有“关闭”

```cpp
void close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    ready_.notify_all();          // 叫醒所有在等的线程
}
```

如果没有 `closed_`，相机停止之后，检测线程会永远睡在 `wait` 里，`join()` 也就永远等不到它结束，程序退不出去。**凡是会让线程等待的地方，都要有办法让它醒来并退出。**

### 这个信箱和设计的关系

这个“只保留最新一帧”的信箱有一个特点：检测跟不上相机时，旧的帧被直接覆盖。这是相机取图时常见的策略。另一种策略是排队，一帧都不丢，但队列可能越积越长。两种各有适用的场景，下一课的推理队列用的是后一种，并且加了容量上限。

## 6. 项目里哪些地方涉及多线程

| 位置 | 现在 | 以后 |
|---|---|---|
| 事件总线 | 已经用互斥锁保护订阅列表 | — |
| 执行计划缓存 | 已经用互斥锁保护 | — |
| 算子目录 | 启动时注册，之后只读，所以不需要锁 | — |
| 值对象（`Image`、`Roi`……） | 不可变，多线程同时读永远安全（第 5 课） | — |
| 工位（POC-9） | 还没实现 | 每个工位一个工作线程；后台线程加载新的上线包；热切换只发生在两次检测之间 |
| 推理 | 还没实现 | 多个工位共用一个推理线程，见下一课 |

注意第 3、4 行：**避免数据竞争最好的办法，是根本不共享可变的数据。** 不可变的对象、只在启动时写一次的数据，都不需要锁。需要锁的地方越少越好。

## 7. 动手运行

**macOS / Linux**：

```bash
clang++ -std=c++17 -g -pthread race.cpp -o race && ./race                          # 结果不对，且每次不同
clang++ -std=c++17 -g -pthread -fsanitize=thread race.cpp -o race_tsan && ./race_tsan   # 报告数据竞争
clang++ -std=c++17 -g -pthread -fsanitize=thread sync.cpp -o sync && ./sync        # 没有问题
```

**Windows**（没有线程检查工具，只能运行）：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 race.cpp && race.exe
cl /std:c++17 /W4 /EHsc /utf-8 sync.cpp && sync.exe
```

## 8. 练习

1. **观察数据竞争**：运行 `race` 十次，记录结果。最小和最大各是多少？然后用 TSan 编译运行，读懂报告。
2. **用两种方式修好**：分别用 `std::mutex` 和 `std::atomic<int>` 修复 `race.cpp`，用 TSan 确认不再报告。用第 14 课的方法测一下哪种更快。
3. **去掉条件试试**：把 `LatestFrame::take` 里的 `ready_.wait(lock, 条件)` 改成不带条件的 `ready_.wait(lock)`，多运行几次。可能出现什么问题？（改回去。）
4. **去掉 close 试试**：把 `main` 里的 `mailbox.close();` 注释掉再运行。程序会怎样？为什么？
5. **写一个线程安全的类**：`class Statistics`，提供 `void record(bool ok)` 和 `double yield() const`（良率 = OK 数 ÷ 总数）。让 4 个线程同时调用 `record`，用 TSan 验证。想一想：为什么这里不能用两个 `std::atomic<int>` 分别保存 OK 数和总数？

## 9. 自测题

1. 什么是数据竞争？为什么它特别难查？
2. `++counter` 为什么不是线程安全的？
3. 为什么用 `std::lock_guard`，而不是手动 `lock()` 和 `unlock()`？
4. 为什么不能拿着锁做耗时的事？项目的事件总线是怎么避免的？
5. 什么是死锁？怎么避免？
6. 条件变量的 `wait` 为什么一定要带条件？
7. 为什么会让线程等待的地方都需要一个“关闭”的机制？
8. 避免数据竞争最根本的办法是什么？

## 下一课

[第 16 课：`promise`、`future` 与生产者消费者——推理服务](../lesson16/README.md)
