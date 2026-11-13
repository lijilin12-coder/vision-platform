# 第 17 课：日志与崩溃转储

> 预计用时：2 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起，以及项目自己的日志和崩溃处理

## 这一课要回答的问题

第 12 课的调试方法有一个前提：你能在自己的电脑上重现问题。但产线上的程序出问题时，往往是这样的：

> “昨天夜里 3 点多，2 号工位的程序自己退出了。早上重启之后一切正常，再也没出现过。”

没法重现，没法接调试器。这时能依靠的只有程序**自己留下的记录**。

1. 程序应该记录什么？怎么记才有用？
2. 程序崩溃的那一刻，能不能把现场保存下来，事后再分析？
3. 拿到一个崩溃转储文件，怎么从里面找出原因？

## 1. 两种记录

| | 回答的问题 | 类比 |
|---|---|---|
| **日志** | 出问题之前，程序做了什么？ | 飞机的飞行记录仪：一直在记 |
| **崩溃转储** | 崩溃的那一刻，程序停在哪里、变量是什么值？ | 事故现场的照片 |

两者缺一不可。只有转储，知道死在哪里，不知道怎么走到那一步的；只有日志，知道之前做了什么，不知道具体死在哪一行。

## 2. 日志

### 为什么不直接用 `std::cout`

| 问题 | 日志系统的做法 |
|---|---|
| 不知道是什么时候打印的 | 每条带时间，精确到毫秒 |
| 分不清重要的和不重要的 | 分级别：`ERROR`、`WARN`、`INFO`、`DEBUG`、`TRACE` |
| 平时不想看细节，排查时又需要 | 运行时调整级别，不用重新编译 |
| 程序一关，输出就没了 | 写到文件里 |
| 一直写，磁盘会满 | 按大小轮转，只保留最近的几个文件 |
| 多个线程同时打印，内容互相穿插 | 加锁，保证每条日志是完整的一行 |
| 崩溃时，缓冲区里还没输出的内容丢失 | 每条都立刻刷新到磁盘 |

### 一条好日志的样子

```
2026-11-12T11:39:00.607Z ERROR [cli] 命令失败 code=authoring.type_mismatch message="端口类型不兼容：judgement → image"
```

对比一下不好的写法：

```
连线失败了！！！
```

好日志回答了五个问题：**什么时候**（时间）、**多严重**（级别）、**哪个部分**（组件）、**发生了什么**（消息）、**具体是哪个**（键值对）。

### 写日志的规则

**消息固定，数据放进键值对。**

```cpp
log::warn("execution", "节点执行失败", {{"node", nodeId}, {"operator", operatorId}, {"reason", message}});   // 好
log::warn("execution", "节点 " + nodeId + " (" + operatorId + ") 执行失败：" + message);                      // 不好
```

第一种写法可以用 `grep "节点执行失败"` 找出所有同类问题，可以按 `node=` 统计哪个节点最常失败。第二种每条都不一样，没法搜索。

**记录 ID。** 检测 ID、方案版本、上线包。有了它们，才能把一条日志和某一次具体的检测、某一个转储文件对应起来。

**级别要选对。**

| 级别 | 含义 | 问自己 |
|---|---|---|
| `ERROR` | 功能没有完成 | 需要有人来处理吗？ |
| `WARN` | 不正常，但能继续 | 如果频繁出现，值得查一查吗？ |
| `INFO` | 重要的正常事件 | 事后想还原“发生过什么”时，需要它吗？ |
| `DEBUG` | 细节 | 只有排查问题时才想看吗？ |

把正常情况记成 `ERROR`，是最常见的错误。日志里满屏都是 `ERROR` 时，真正的错误就找不到了。

**不要在循环的最里层写日志。** 每条日志要格式化字符串、加锁、写磁盘。每次检测写几条没有问题，每个像素写一条会让程序慢成百上千倍。

**日志里不要放敏感信息和大块数据。**

### 项目里的日志

打开 [`core/common/include/vision/common/log.h`](../../../../core/common/include/vision/common/log.h)。整个设计用的都是前面学过的东西：

| 设计 | 用到的 | 课 |
|---|---|---|
| `LogSink` 是一个接口，控制台和文件是两种实现 | 接口与依赖倒置 | 8 |
| `Logger` 用互斥锁保护接收器列表 | 互斥锁、`lock_guard` | 15 |
| 接收器用 `std::shared_ptr` 持有 | 智能指针 | 4 |
| `LogFields` 用初始化列表传键值对 | 标准容器 | — |
| 文件超过大小就轮转，只保留有限个 | “会增长的东西都要有上限” | 13 |
| 领域层不写日志，由应用层和事件订阅者来写 | 分层 | 8 |

最后一条值得多说一句。看 [`apps/cli/main.cpp`](../../../../apps/cli/main.cpp) 里的这几行：

```cpp
bus.subscribe("*", [this](const EventPtr& e) {
    log::info("event", e->name());
    ...
});
```

它订阅了所有领域事件，每个事件写一条日志。**领域层只管发布“发生了什么”，完全不知道日志的存在。** 这样日志里自然就有了一条业务时间线：

```
INFO  [event] RecipeCreated
INFO  [event] RecipeVersionPublished
INFO  [event] InspectionCompleted
```

### 动手试一试

```bash
CLI=./build/apps/vision-cli
$CLI --data /tmp/demo sample-images /tmp/demo/images
RID=$($CLI --data /tmp/demo recipe build samples/recipes/bright-spot.draft.json)
VID=$($CLI --data /tmp/demo recipe publish $RID)
$CLI --data /tmp/demo --log-level debug inspect $VID /tmp/demo/images
$CLI --data /tmp/demo recipe connect $RID n5.judgement n2.image      # 故意连一条不合法的线
cat /tmp/demo/logs/vision-cli.log
```

## 3. 崩溃转储

### 是什么

程序崩溃时，操作系统可以把这个进程当时的**全部内存和每个线程的状态**保存成一个文件。之后用调试器打开它，就像程序正停在崩溃的那一行：可以看调用栈，看每一层的局部变量，看其他线程在做什么。唯一的区别是不能继续运行。

| 平台 | 名称 | 怎么产生 |
|---|---|---|
| Linux、macOS | **core 文件**（core dump） | 系统自动生成，但默认常常是关闭的 |
| Windows | **minidump**（`.dmp`） | 程序自己调用系统接口写出来 |

“core” 这个词来自几十年前的磁芯存储器，和 CPU 的“核”没有关系。

### 练习：从 core 文件找出原因

[`crash.cpp`](crash.cpp) 模拟连续检测 4 帧图像，在第 3 帧崩溃。

**先不要看源代码**，只用转储文件来找原因。这才是真实的情形：你面对的是几万行代码和一个转储文件。

#### 生成 core 文件（macOS / Linux）

```bash
clang++ -std=c++17 -g -O0 crash.cpp -o crash
```

```bash
ulimit -c unlimited
```

```bash
./crash
```

```
frame 2: 超出 30
frame 3: 开始判定
Segmentation fault (core dumped)
```

`ulimit -c unlimited` 允许生成任意大小的 core 文件，只对当前这个终端有效。看到 `(core dumped)` 说明已经生成了。

- Linux：一般在当前目录下，名字是 `core` 或 `core.<进程号>`。如果没有，执行 `cat /proc/sys/kernel/core_pattern` 看系统把它存到了哪里；使用 systemd 的系统上用 `coredumpctl list`。
- macOS：在 `/cores/` 目录下。

日志（这里是 `cerr` 的输出）已经给了第一条线索：**第 3 帧开始判定之后崩溃的，前两帧正常。**

#### 打开 core 文件

Linux：

```bash
gdb ./crash core
```

macOS：

```bash
lldb ./crash -c /cores/core.<进程号>
```

Windows 上没有 core 文件。在 Visual Studio 里直接按 F5 运行这个程序，崩溃时会停在出错的那一行，之后的分析步骤相同。

#### 第一步：看调用栈

```
(gdb) bt
#0  std::vector<Blob, std::allocator<Blob> >::size (this=0x0) at /usr/include/c++/13/bits/stl_vector.h:993
#1  largestBlob (ctx=...) at crash.cpp:28
#2  judge (ctx=..., maxArea=50) at crash.cpp:35
#3  runInspection (frame=3, upstreamFailed=true) at crash.cpp:46
#4  main () at crash.cpp:53
```

最上面的 `#0` 是崩溃的位置：在标准库 `vector` 的 `size()` 里面。

**标准库没有 bug。** 调用栈最上面一层只是**表现出症状的地方**。要往下找**第一行属于自己代码**的位置：`#1 largestBlob`，`crash.cpp` 第 28 行。

再看 `#0` 那一行的 `this=0x0`：对一个**空指针**调用了 `size()`。

#### 第二步：看那一层的变量

```
(gdb) frame 1
#1  largestBlob (ctx=...) at crash.cpp:28
28	    for (std::size_t i = 1; i < blobs.size(); ++i) {

(gdb) print ctx.blobs
$1 = (const std::vector<Blob, std::allocator<Blob> > *) 0x0

(gdb) print ctx.recipe
$2 = "bright-spot@v1"
```

`ctx.blobs` 是空指针。

#### 第三步：往上找，它为什么是空的

```
(gdb) frame 3
#3  runInspection (frame=3, upstreamFailed=true) at crash.cpp:46

(gdb) print upstreamFailed
$4 = true
```

`upstreamFailed=true`：这一帧的上游节点失败了。

到这里结论已经清楚了，**全程没有看一行源代码**：

> 第 3 帧的上游节点失败，没有产出 Blob 列表，`ctx.blobs` 是空指针。`largestBlob` 没有检查就直接使用了它。

现在再打开源代码确认，然后修复。

#### 修复

直接加一个空指针检查能解决崩溃，但更好的做法是用前面学过的方法，让这个错误**写不出来**：

- `largestBlob` 的参数改成 `const std::vector<Blob>&`。引用不能为空，调用者必须先处理“没有”的情况（第 3 课）。
- 或者返回 `std::optional<Blob>`，把“没有”变成类型的一部分（第 6 课）。
- 项目里的做法：上游节点失败时，执行引擎直接把下游节点标记为“跳过”，根本不会调用它（不变量 I2）。

### 常用命令

| 作用 | GDB | LLDB |
|---|---|---|
| 调用栈 | `bt` | `bt` |
| 切换到第 N 层 | `frame N` | `frame select N` |
| 这一层的局部变量 | `info locals` | `frame variable` |
| 查看变量或表达式 | `print 表达式` | `p 表达式` |
| 所有线程 | `info threads` | `thread list` |
| 所有线程的调用栈 | `thread apply all bt` | `bt all` |

多线程程序卡死或崩溃时，**一定要看所有线程的调用栈**。出问题的原因常常在另一个线程里。

### 必须保存调试信息

分析转储文件需要**同一次构建**产生的程序和调试信息。重新编译一遍，哪怕源代码一个字没改，地址也可能对不上。

所以发到产线的每一个版本，都要把程序和调试信息存档：

| 平台 | 调试信息在哪里 |
|---|---|
| Linux、macOS | 用 `-g` 编译时在程序文件里（macOS 上也可能在旁边的 `.dSYM` 目录里） |
| Windows | 单独的 `.pdb` 文件 |

发布版本一样可以带调试信息：`-O2 -g`（Windows 上是 `/O2 /Zi`），CMake 里是 `RelWithDebInfo`。优化后的程序里，有些变量会显示 `<optimized out>`，调用栈也可能少几层，但通常仍然足够定位问题。

## 4. 项目里的崩溃处理

系统默认的行为不够可靠：Linux 上 core 文件常常默认关闭；Windows 上默认弹出一个对话框，然后什么都不留下。所以项目在启动时安装了自己的崩溃处理（[`adapters/crash/src/crash_handler.cpp`](../../../../adapters/crash/src/crash_handler.cpp)）。

| 平台 | 做了什么 |
|---|---|
| Windows | 崩溃时写 minidump 和一份文本报告；关闭崩溃对话框 |
| macOS / Linux | 崩溃时写文本报告（信号、出错地址、调用栈）；放开 core 文件的大小限制 |
| 所有平台 | 未捕获的异常：先把异常内容写进日志，再按崩溃处理 |

**关闭对话框**这一点很重要。无人值守的产线上，一个等人点“确定”的对话框会让程序一直挂在那里，守护进程以为它还活着，不会去重启它。

### 信号处理函数里能做的事很少

打开 `crash_handler.cpp` 的 macOS / Linux 部分，会发现它没有用 `std::string`、`printf`、`std::ofstream`，而是自己写了把数字转成文本的函数，只调用 `open`、`write`、`close`。

原因是：崩溃可能发生在任何时刻，比如程序正在申请内存、内存管理的锁还没有释放的时候。如果崩溃处理函数里再去申请内存，就会等那把永远不会释放的锁，程序卡死，什么都留不下。

所以信号处理函数里**不能申请内存、不能加锁、不能用绝大多数库函数**。崩溃报告的文件路径也是在安装时就提前准备好的。

这也是上一课和第 15 课内容的延伸：凡是涉及“一段代码可能在任意时刻打断另一段代码”，都要格外小心。

### 验证它确实能用

崩溃处理是那种“平时用不到，要用的时候必须能用”的代码。所以项目里有专门的测试：

```bash
./build/apps/vision-cli --data /tmp/demo debug crash segv
```

`segv` 可以换成 `abort`、`throw`。测试 `crash_leaves_report_*` 会故意让程序以这三种方式崩溃，然后检查：程序以非零退出码结束；崩溃目录里有崩溃报告（Windows 上还有非空的 minidump）；日志里有崩溃前写下的最后一条记录。

完整的使用说明见 [日志与崩溃转储](../../../dev/logging-and-crash-dumps.md)。

## 5. 一次完整的事后分析

把这一课的内容串起来。回到开头那个问题：“昨天夜里 3 点多，2 号工位的程序自己退出了。”

1. **看崩溃目录。** 有没有那个时间的崩溃报告？有，说明是崩溃；没有，说明是被外部结束的（断电、被系统杀掉、有人关了它）。
2. **看崩溃报告。** 什么信号？出错地址是不是 `0x0`（空指针）？调用栈指向哪个模块？
3. **看日志里崩溃前的最后几十行。** 当时在处理哪次检测？用的哪个上线包？之前有没有 `WARN`？是不是刚刚热切换过？
4. **打开转储文件。** 看调用栈，找到第一行自己的代码，看变量。
5. **形成猜想，在自己的电脑上重现。** 根据日志里的方案版本和检测 ID，找到当时的图像和方案，重新运行。
6. **修复，并留下测试。**

第 5 步是设计文档里“可追溯”（NFR-06）的用处：每次检测都记录了它用的上线包、方案版本、模型版本、标定档案，图像缓存里还留着最近的图像。有了这些，现场的问题才能搬到开发机上重现。

## 6. 动手运行

**macOS / Linux**（会生成 core 文件）：

```bash
clang++ -std=c++17 -g -O0 crash.cpp -o crash && ulimit -c unlimited && ./crash
```

**Windows**：

```bat
cl /std:c++17 /EHsc /utf-8 /Zi /Od crash.cpp
```

然后在 Visual Studio 里打开 `crash.exe`（“文件 → 打开 → 项目/解决方案”，选择这个 exe），按 F5 运行。

## 7. 练习

1. **完成第 3 节的分析**：生成 core 文件，用调试器打开，不看源代码，只靠调用栈和变量找出原因。
2. **用三种方式修复**：分别用“改成引用参数”“返回 `std::optional`”“调用前检查”修好 `crash.cpp`。比较三种做法：哪一种能让同样的错误以后写不出来？
3. **让项目崩溃一次**：运行 `vision-cli --data /tmp/demo debug crash throw`。在 `/tmp/demo/crashes/` 和 `/tmp/demo/logs/` 里找到留下的文件。日志的最后一行是什么？它是哪段代码写的？
4. **分析项目的转储**：运行 `debug crash segv`，用调试器打开生成的 core 文件（Windows 上是 `.dmp`）。调用栈里从 `main` 到崩溃点一共经过了哪几个函数？
5. **给日志加上下文**：在 [`execution_service.cpp`](../../../../core/contexts/execution/application/src/execution_service.cpp) 里，“检测完成”这条日志目前没有记录节点数量。加上一个字段 `nodes`，重新编译，用 `--log-level debug` 运行一次检测，确认日志里出现了它。
6. **判断级别**：下面的情况各应该用哪个级别？
   - 工位热切换到了新的上线包
   - 一次检测判定为 NG
   - 一次检测超过了节拍
   - 模型文件加载失败，工位无法启动
   - 每个节点的执行耗时
   - 标定档案还有 3 天过期

## 8. 自测题

1. 日志和崩溃转储分别回答什么问题？为什么两者都需要？
2. 相比直接 `std::cout`，日志系统多做了哪些事？
3. 为什么日志的消息要固定，把变化的数据放进键值对？
4. 日志文件为什么要轮转？
5. 崩溃转储文件里保存了什么？打开它之后能做什么、不能做什么？
6. 调用栈最上面一层是标准库里的函数，说明标准库有 bug 吗？应该怎么看？
7. 为什么发布到产线的每个版本都要存档调试信息？
8. 信号处理函数里为什么不能申请内存、不能加锁？
9. 产线上为什么要关闭 Windows 的崩溃对话框？

## 教程到这里结束

这是最后一课。前 16 课讲的是怎么把程序写对；这一课讲的是程序在你看不见的地方出了问题时，怎么把它查出来。两者合在一起，才是一个能放到产线上运行的程序。
