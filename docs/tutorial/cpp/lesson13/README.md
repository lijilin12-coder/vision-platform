# 第 13 课：内存分析

> 预计用时：2 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

产线上的检测程序要 7×24 小时不间断运行。内存问题在这种场景下是致命的：越界可能悄悄算错结果；每次检测漏掉 4 KB 内存，一天下来就是几个 GB。

1. C++ 里常见的内存错误有哪几种？
2. 用什么工具发现它们？
3. 内存没有“泄漏”，却一直在涨，是怎么回事？

## 1. 内存错误的种类

| 错误 | 是什么 | 后果 |
|---|---|---|
| **越界** | 读写了数组范围之外的内存 | 读到垃圾数据；写坏了别的变量；偶尔崩溃 |
| **释放后使用** | 内存已经释放，还通过旧的指针或引用去访问 | 同上。第 3 课的悬空引用就是这一种 |
| **重复释放** | 同一块内存释放了两次 | 通常崩溃。第 1 课的 `BrokenString` |
| **泄漏** | 申请了内存，再也没有释放，也没有任何指针指着它了 | 内存越用越多 |
| **未初始化** | 读了一个没有赋过值的变量 | 结果随机，有时对有时错 |
| **未定义行为** | 语言标准规定“不许这样做”的操作，比如有符号整数溢出 | 什么都可能发生，尤其在开了优化之后 |

这些错误最麻烦的地方是：**出错的地方和表现出症状的地方往往离得很远。** 在 A 处越界写坏了一块内存，程序可能继续运行很久，直到 B 处用到那块内存才崩溃。只看 B 处的代码，永远找不到原因。

所以要靠工具，在错误**发生的那一刻**就抓住它。

## 2. 内存检查工具：AddressSanitizer

AddressSanitizer（简称 ASan）是编译器自带的工具。加一个编译选项，程序里的每次内存访问都会被检查。程序会慢 2 倍左右，内存多用 2～3 倍，所以只在开发和测试时用。

| 编译器 | 选项 |
|---|---|
| Clang / GCC | `-g -fsanitize=address` |
| MSVC | `/Zi /fsanitize=address` |

### 越界

[`overflow.cpp`](overflow.cpp)：

```cpp
void copyRow(const std::vector<unsigned char>& row, unsigned char* dest) {
    for (std::size_t i = 0; i <= row.size(); ++i) {   // ← 应该是 <，写成了 <=
        dest[i] = row[i];
    }
}
```

不开检查时，程序正常结束，退出码是 0，看不出任何问题：

```
复制完成，最后一个像素 = 128
```

开了 ASan：

```
ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...
    #0 ... in copyRow(...) .../overflow.cpp:9
    #1 ... in main .../overflow.cpp:16
```

### 怎么读 ASan 的报告

报告很长，只需要看三处：

1. **第一行**：错误的类型。

   | 类型 | 含义 |
   |---|---|
   | `heap-buffer-overflow` | 堆上的越界 |
   | `stack-buffer-overflow` | 栈上的越界（局部数组） |
   | `heap-use-after-free` | 释放后使用 |
   | `attempting double-free` | 重复释放 |
   | `stack-use-after-scope` | 用到了已经离开作用域的局部变量 |

2. **第一段调用栈**：出错的访问发生在哪里。从上往下找到**第一行属于你自己代码**的位置。
3. **后面的 “allocated by” / “freed by”**：这块内存是在哪里申请的、在哪里释放的。对“释放后使用”特别有用。

## 3. 泄漏

[`leak.cpp`](leak.cpp) 里有两种泄漏。

### 第一种：`new` 了没有 `delete`

```cpp
bool loadAndCheck(bool valid) {
    Model* model = new Model();
    if (!valid) return false;     // ← 提前返回，忘了 delete
    delete model;
    return true;
}
```

修复：用 `std::make_unique`（第 4 课）。

### 第二种：`shared_ptr` 互相指着对方

```cpp
struct Node {
    std::shared_ptr<Node> next;   // 上游指向下游
    std::shared_ptr<Node> prev;   // 下游又指回上游 ← 形成了环
};

a->next = b;
b->prev = a;
```

`a` 被 `b->prev` 持有，`b` 被 `a->next` 持有。函数结束后，两个引用计数都还是 1，永远不会归零。**`shared_ptr` 不能处理循环引用**，这是它最大的陷阱。

修复：让其中一个方向不参与计数，用 `std::weak_ptr`（[`leak_fixed.cpp`](leak_fixed.cpp)）：

```cpp
struct Node {
    std::shared_ptr<Node> next;   // “拥有”下游
    std::weak_ptr<Node> prev;     // 只是“认识”上游，不增加计数
};

if (const auto upstream = b->prev.lock()) { ... }   // 用之前先 lock()：对象还在才能用
```

设计时先想清楚**谁拥有谁**：拥有关系必须是单向的，不能成环。反方向的关系用 `weak_ptr`、裸指针或 ID。

本项目的做法更彻底：**聚合之间只用 ID 引用**（设计文档 05 §0）。节点之间的连线保存的是节点 ID，不是指针，所以不可能出现循环引用。

### 用工具发现泄漏

各平台的工具不一样：

| 平台 | 工具 | 用法 |
|---|---|---|
| Linux | LeakSanitizer，包含在 ASan 里 | 用 `-fsanitize=address` 编译，运行结束时自动报告 |
| macOS | 系统自带的 `leaks` | `leaks --atExit -- ./leak` |
| Windows | Visual Studio 的“诊断工具” | 调试时打开“内存使用率”，在两个时间点各拍一次快照，对比增加了哪些对象 |

在 Linux 上的报告（节选）：

```
ERROR: LeakSanitizer: detected memory leaks

Direct leak of 24 byte(s) in 1 object(s) allocated from:
    #1 ... in loadAndCheck(bool) .../leak.cpp:14

Indirect leak of 4000 byte(s) in 1 object(s) allocated from:
    #7 ... in Model::Model() .../leak.cpp:9
    #8 ... in loadAndCheck(bool) .../leak.cpp:14

Indirect leak of 80 byte(s) in 1 object(s) allocated from:
    #8 ... in buildCycle() .../leak.cpp:30
```

它指出了每一块泄漏的内存是在**哪一行申请的**。“Direct” 是直接泄漏的对象，“Indirect” 是只能通过它才能访问到的内存（这里是 `Model` 里面的权重数组）。

本项目的自动构建在 Linux 上用 ASan 运行教程示例，所以泄漏在那里会被发现。

## 4. 未定义行为：UBSan

[`int_overflow.cpp`](int_overflow.cpp)：

```cpp
int imageBytes(int width, int height, int channels) {
    return width * height * channels;     // int 最大约 21 亿
}
```

一张 30000×30000 的三通道图像需要 27 亿字节，超出了 `int` 的范围。有符号整数溢出是未定义行为。

用 `-fsanitize=undefined`（UBSan）编译运行：

```
int_overflow.cpp:7:29: runtime error: signed integer overflow: 3 * 900000000 cannot be represented in type 'int'
正确的字节数：2700000000
用 int 计算： -1594967296
```

算出来是一个**负数**。拿它去申请内存或者当下标，后果不堪设想。图像处理的代码里，“宽 × 高 × 通道数”是最容易溢出的地方。

修复：先转换成 `std::size_t` 再相乘。项目里 [`kernel.cpp`](../../../../core/kernel/src/kernel.cpp) 的 `Image::create` 就是这样做的：

```cpp
const std::size_t expected = static_cast<std::size_t>(width) * height * channels;
```

ASan 和 UBSan 可以一起开：`-fsanitize=address,undefined`。MSVC 没有 UBSan，这类问题靠 Linux 上的自动构建来发现。

## 5. 没有泄漏，内存却一直涨

[`growth.cpp`](growth.cpp)：

```cpp
class UnboundedHistory {
public:
    void add(InspectionRecord record) { records_.push_back(std::move(record)); }   // 只进不出
private:
    std::vector<InspectionRecord> records_;
};
```

每次检测都把记录追加进去，从不清理。每条记录带一张 4 KB 的缩略图：

```
不清理：保留了 5000 条，约 19 MB
只保留最近 100 条：保留了 100 条，最旧的是第 4900 条
```

5000 次检测只是每秒 20 次时 4 分钟的量。按这个速度，一天会涨到 6 GB 以上。

**这不是泄漏。** 每一块内存都还有人指着，程序结束时也会正常释放，所以任何泄漏检查工具都不会报告它。但对一个长期运行的程序来说，效果和泄漏一模一样。

常见的来源：

| 来源 | 修复 |
|---|---|
| 历史记录、日志缓存只进不出 | 设上限，只保留最近 N 条 |
| 缓存没有淘汰策略 | 设容量或过期时间 |
| 生产者比消费者快，队列越积越长 | 有界队列加背压（第 16 课） |
| 事件订阅了却从不取消 | 对象销毁时取消订阅 |

设计时有一条简单的规则：**凡是会增长的容器，都要回答“它什么时候变小”。** 答不上来，就是隐患。

本项目的设计里已经考虑了这一点：图像缓存有保留期（F-3），检测记录需要保留策略，执行计划的缓存也要在 MVP 阶段加上限。

### 怎么发现

这类问题靠**观察内存随时间的变化**：

| 平台 | 工具 |
|---|---|
| Windows | 任务管理器看趋势；Visual Studio “诊断工具”拍两次快照做对比，直接看到哪种对象变多了 |
| macOS | 活动监视器看趋势；Instruments 的 Allocations 模板 |
| Linux | `top` 看 RES 一列；`valgrind --tool=massif` 记录堆的增长过程，再用 `ms_print` 查看 |

做法都一样：让程序跑一段时间，在两个时间点各记录一次，看多出来的内存是谁申请的。

POC 计划里的 POC-8（鸿蒙电脑上连续检测 8 小时以上，内存不能持续增长）验证的就是这件事。

## 6. 日常怎么用

| 什么时候 | 做什么 |
|---|---|
| 平时开发 | 调试版本一直开着 ASan。有错当场就知道 |
| 提交之前 | 用 ASan 版本跑一遍全部测试 |
| 自动构建 | Linux 上用 ASan 和 UBSan 运行 |
| 上线之前 | 长时间运行测试，观察内存曲线是否平稳 |

给项目开启 ASan（macOS / Linux）：

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
```

```bash
cmake --build build-asan && ctest --test-dir build-asan
```

## 7. 最好的办法是不给它机会

工具只能发现**运行到了**的错误。更可靠的是用前几课的写法，让这些错误根本写不出来：

| 错误 | 让它写不出来的写法 | 课 |
|---|---|---|
| 泄漏、重复释放 | 不写 `new` / `delete`，用智能指针和标准容器 | 4 |
| 越界 | 用范围 `for` 和算法，不写下标循环；需要下标时用 `.at()` | 9 |
| 释放后使用 | 值对象按值传递；容器变动后不保留旧引用 | 3、5 |
| 未初始化 | 成员变量在声明处给默认值；让非法状态创建不出来 | 5 |
| 循环引用 | 用 ID 引用，而不是互相持有指针 | — |
| 整数溢出 | 尺寸和下标用 `std::size_t`，相乘之前先转换 | — |

## 8. 动手运行

**macOS / Linux**：

```bash
F="-std=c++17 -Wall -Wextra -g"

clang++ $F -fsanitize=address overflow.cpp -o overflow && ./overflow          # 报告 heap-buffer-overflow
clang++ $F -fsanitize=undefined int_overflow.cpp -o int_overflow && ./int_overflow   # 报告整数溢出
clang++ $F -fsanitize=address leak_fixed.cpp -o leak_fixed && ./leak_fixed    # 没有问题
clang++ $F growth.cpp -o growth && ./growth
```

泄漏示例，Linux：

```bash
g++ -std=c++17 -g -fsanitize=address leak.cpp -o leak && ./leak
```

泄漏示例，macOS：

```bash
clang++ -std=c++17 -g leak.cpp -o leak && leaks --atExit -- ./leak
```

**Windows**：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 /Zi /fsanitize=address overflow.cpp && overflow.exe
cl /std:c++17 /W4 /EHsc /utf-8 leak_fixed.cpp && leak_fixed.exe
cl /std:c++17 /W4 /EHsc /utf-8 growth.cpp && growth.exe
```

## 9. 练习

1. **读报告**：运行 `overflow`，在 ASan 的报告里找到：错误类型、出错的行号、这块内存是在哪一行申请的、越界了几个字节。然后修好它。
2. **对比开和不开**：不带 `-fsanitize` 编译 `overflow.cpp` 并运行。程序报错了吗？这说明了什么？
3. **修泄漏**：不看 `leak_fixed.cpp`，自己把 `leak.cpp` 的两处泄漏修好，用工具确认不再报告。
4. **制造一个释放后使用**：写一个函数，返回 `std::vector<int>` 里第一个元素的**指针**，然后让那个 `vector` 离开作用域，再通过指针读取。用 ASan 运行，读懂报告里的 “freed by” 部分。
5. **给增长设上限**：`growth.cpp` 的 `RecentHistory` 按“条数”限制。改写成按“总字节数”限制：超过 1 MB 时丢弃最旧的记录。
6. **检查项目**：在项目里搜索成员变量是 `std::map` 或 `std::vector` 的类（比如 [`execution_service.cpp`](../../../../core/contexts/execution/application/src/execution_service.cpp) 里的 `planCache`）。它们什么时候变小？哪些有无限增长的风险？

## 10. 自测题

1. 内存错误为什么难查？
2. ASan 能发现哪些错误？它的代价是什么？
3. ASan 的报告里，最先看哪三处？
4. 两个对象用 `shared_ptr` 互相持有对方，会发生什么？怎么解决？
5. “内存一直在涨，但泄漏检查工具没有报告任何问题”，可能是什么原因？
6. 为什么“宽 × 高 × 通道数”要先转换成 `std::size_t` 再相乘？
7. 除了用工具检查，更根本的办法是什么？

## 下一课

[第 14 课：性能分析](../lesson14/README.md)
