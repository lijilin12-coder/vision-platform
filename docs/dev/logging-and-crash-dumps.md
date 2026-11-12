# 日志与崩溃转储

产线程序 7×24 小时运行，出问题时人通常不在现场。事后排查靠两样东西：

| | 回答的问题 | 什么时候产生 |
|---|---|---|
| **日志** | 出问题之前，程序做了什么？ | 一直在写 |
| **崩溃转储** | 崩溃的那一刻，程序停在哪里、各个变量是什么值？ | 崩溃时 |

两者配合使用：转储告诉你**死在哪里**，日志告诉你**怎么走到那里的**。

## 1. 文件在哪里

都在数据目录下（默认 `./vision-data`，用 `--data` 指定）：

```
<数据目录>/
├── logs/
│   ├── vision-cli.log          当前日志
│   ├── vision-cli.1.log        上一个（更旧的是 .2、.3……）
│   └── ...
└── crashes/
    ├── crash-vision-cli-20261112-113900-462.txt     崩溃报告（所有平台）
    └── crash-vision-cli-20261112-113900-462.dmp     minidump（仅 Windows）
```

崩溃文件名里是进程的启动时间（UTC）和进程号。macOS 和 Linux 上的 core 文件由系统决定存放位置，见第 4 节。

## 2. 日志

### 格式

每条日志一行：

```
2026-11-12T11:39:00.405Z INFO  [event] RecipeVersionPublished
2026-11-12T11:39:00.484Z DEBUG [execution] 检测完成 inspection=cb66c3b7-… recipe=e3d0aa9c-…@v1 purpose=production verdict=NG ms=2.33
2026-11-12T11:39:00.607Z ERROR [cli] 命令失败 code=authoring.type_mismatch message="端口类型不兼容：judgement → image"
```

| 部分 | 说明 |
|---|---|
| 时间 | UTC，精确到毫秒。用 UTC 是为了不同机器、不同时区的日志可以直接对照 |
| 级别 | `TRACE`、`DEBUG`、`INFO`、`WARN`、`ERROR` |
| `[组件]` | 日志来自哪里 |
| 消息 | 一句固定的话 |
| `键=值` | 会变化的数据。值里有空格、引号时会加引号；换行会被转义，**保证一条日志始终是一行** |

消息固定、数据放在键值对里，是为了方便搜索：

```bash
grep "节点执行失败" vision-cli.log            # 找出所有节点失败
grep "inspection=cb66c3b7" vision-cli.log    # 找出某一次检测的全部日志
grep -c " ERROR " vision-cli.log             # 统计错误数量
```

### 级别

| 级别 | 用于 | 例子 |
|---|---|---|
| `ERROR` | 出错了，功能没有完成 | 命令失败；未捕获的异常 |
| `WARN` | 不正常，但程序还能继续 | 节点执行失败（这次检测判为 ERROR，下一次照常进行） |
| `INFO` | 重要的正常事件，数量不多 | 启动；每个领域事件 |
| `DEBUG` | 排查问题时才需要的细节，数量大 | 每次检测的结果和耗时 |
| `TRACE` | 极其详细，只在追查具体问题时临时打开 | |

默认级别是 `INFO`。排查问题时用 `--log-level debug` 临时调低。

### 轮转

单个日志文件最大 5 MB，超过后改名为 `.1.log`，原来的 `.1` 变成 `.2`，最多保留 5 个旧文件。所以**日志占用的磁盘空间有上限（约 30 MB）**，长期运行不会把磁盘写满。

每写一条就刷新到磁盘。这样程序崩溃时，崩溃前的最后几条日志不会留在缓冲区里丢掉。

### 在代码里写日志

```cpp
#include "vision/common/log.h"

vision::log::info("station", "热切换完成", {{"from", "R-1"}, {"to", "R-2"}});
vision::log::warn("execution", "节点执行失败", {{"node", nodeId}, {"reason", message}});
```

规则：

| 规则 | 原因 |
|---|---|
| **领域层不写日志** | 领域层不依赖任何技术。日志由应用层和适配器来写，或者由订阅领域事件的代码来写 |
| **消息是固定的一句话，变化的数据放进键值对** | 方便搜索和统计 |
| **不要在处理像素的循环里写日志** | 日志的粒度是“每次检测发生了什么”。每条日志都要格式化、加锁、写磁盘 |
| **不要把大块数据写进日志** | 图像、整个方案的 JSON 不属于日志；记录它们的 ID 即可 |
| **记录 ID** | 检测 ID、方案版本、上线包：事后可以把日志和检测记录、转储文件对应起来 |
| **级别要选对** | 把正常情况记成 `ERROR`，真正的错误就会被淹没 |

### 实现

| 部分 | 位置 |
|---|---|
| 接口和分发：`Logger`、`LogSink`、`formatLogLine` | [`core/common/include/vision/common/log.h`](../../core/common/include/vision/common/log.h) |
| 接收器：控制台、轮转文件 | [`adapters/logging/`](../../adapters/logging/) |
| 配置（日志目录、级别） | 组装根，[`apps/cli/main.cpp`](../../apps/cli/main.cpp) |

日志写到哪里由**接收器**决定。鸿蒙上可以增加一个写入系统日志（hilog）的接收器，其余代码不用改。

## 3. 崩溃时会发生什么

程序启动时安装了崩溃处理（[`adapters/crash/`](../../adapters/crash/)）。发生下面任何一种情况时，它会留下现场：

| 情况 | 例子 |
|---|---|
| 非法内存访问 | 对空指针取值、越界 |
| `abort()` | 断言失败 |
| 未捕获的异常 | 异常一路传到了 `main` 之外。**异常的内容会先写进日志** |
| 其他致命信号 | 整数除以零、非法指令 |

| 平台 | 留下什么 |
|---|---|
| Windows | 崩溃报告 `.txt`，加上 **minidump** `.dmp`。不会弹出对话框：无人值守时，对话框会让程序一直卡在那里，没法自动重启 |
| macOS / Linux | 崩溃报告 `.txt`（信号、出错地址、调用栈），并放开 core 文件的大小限制，让系统生成 **core 文件** |
| 鸿蒙 | 待 POC 验证。系统自己会记录崩溃日志；我们的崩溃报告在鸿蒙上没有调用栈（它的 C 库不提供这个功能） |

可以用调试命令验证这套机制是否正常：

```bash
./build/apps/vision-cli --data /tmp/demo debug crash segv
```

`segv` 可以换成 `abort` 或 `throw`。项目的测试里有三个测试专门做这件事：故意让程序崩溃，然后检查崩溃报告、转储文件和日志是否如预期留下。

## 4. 分析崩溃

### 第一步：看崩溃报告和日志

macOS / Linux 上的崩溃报告：

```
vision crash report
signal: 11 (SIGSEGV) invalid memory access
fault address: 0x0
pid: 462
backtrace:
vision-cli(+0x2afea4)[0x55a28c514ea4]
...
vision-cli(+0x20bcd6)[0x55a28c470cd6]
vision-cli(+0x20bd16)[0x55a28c470d16]
vision-cli(main+0xb0d)[0x55a28c4757fd]
```

`fault address: 0x0` 说明访问了空指针。调用栈里有些行只有地址，可以换算成函数名和行号（需要同一次构建出来的程序）：

```bash
addr2line -e build/apps/vision-cli -f -C -p 0x20bcd6 0x20bd16
```

```
(anonymous namespace)::crashWriteThroughNull(int volatile*) at apps/cli/main.cpp:108
(anonymous namespace)::crashInOperator(std::string const&) at apps/cli/main.cpp:117
```

然后看日志的最后几行，了解崩溃前程序在做什么：

```
2026-11-12T11:39:00.753Z WARN  [cli] 即将按要求故意崩溃 kind=throw
2026-11-12T11:39:00.754Z ERROR [crash] 未捕获的异常，程序即将终止 what=这是一个故意抛出、没有人捕获的异常
```

很多时候到这里就够了。

### 第二步：打开转储文件

转储文件是崩溃那一刻整个进程的快照，可以像在调试器里停在断点上一样查看调用栈和变量。

**必须用同一次构建产生的程序和调试信息。** 重新编译过的程序，地址和行号都对不上。所以发布到产线的每个版本，都要把程序和调试信息（Windows 上是 `.pdb` 文件）存档。

#### Linux：core 文件

```bash
gdb build/apps/vision-cli /path/to/core
(gdb) bt
```

```
#0  (anonymous namespace)::crashWriteThroughNull (target=0x0) at apps/cli/main.cpp:108
#1  (anonymous namespace)::crashInOperator (kind="segv") at apps/cli/main.cpp:111
#2  (anonymous namespace)::crashInInspection (kind="segv") at apps/cli/main.cpp:118
#3  (anonymous namespace)::cmdDebugCrash (kind="segv") at apps/cli/main.cpp:126
#4  main (argc=6, argv=0x7fff176bb308) at apps/cli/main.cpp:407
```

常用命令：`bt`（调用栈）、`frame 2`（切换到第 2 层）、`info locals`（这一层的局部变量）、`print 变量`、`info threads` 和 `thread apply all bt`（所有线程的调用栈）。

core 文件存在哪里由系统设置决定：

```bash
cat /proc/sys/kernel/core_pattern
```

- 输出是 `core` 之类的文件名：存在程序的当前目录下。
- 输出以 `|` 开头：交给了系统的崩溃收集服务。使用 systemd 的系统上用 `coredumpctl list` 查看，`coredumpctl gdb` 直接打开最近的一个。

程序启动时已经把 core 文件的大小限制放开了。如果仍然没有生成，检查硬限制 `ulimit -Hc` 是不是 0。

#### macOS：core 文件和系统崩溃报告

```bash
lldb build/apps/vision-cli -c /cores/core.<进程号>
(lldb) bt
```

core 文件在 `/cores/` 目录下。macOS 还会自己生成一份崩溃报告，在 `~/Library/Logs/DiagnosticReports/`，可以用“控制台”应用查看，里面有每个线程的调用栈。

> macOS 上的这些步骤还没有实际验证过。

#### Windows：minidump

用 Visual Studio：“文件 → 打开 → 文件”，选择 `.dmp`，然后点“使用仅限本机进行调试”。Visual Studio 会停在崩溃的位置，可以查看调用堆栈、局部变量，和调试时一样。

需要对应的 `.pdb` 文件。如果程序就是在本机编译的，Visual Studio 能自动找到；否则在“调试 → 选项 → 符号”里添加 `.pdb` 所在的目录。

也可以用 WinDbg：打开 `.dmp` 后执行 `!analyze -v`。

> Windows 上 minidump 能否正确生成，由自动构建里的测试检查（文件存在且非空）。用 Visual Studio 打开它的步骤还没有实际验证过。

### 第三步：定位原因

调用栈最上面的一层不一定是出错的原因，它只是**表现出症状的地方**。往下找到**第一行属于自己代码**的位置，再看那里的变量。教程第 17 课有一个完整的例子。

## 5. 不生成 core 文件

core 文件可能很大（和程序占用的内存相当）。不需要时：

```bash
./build/apps/vision-cli --no-core-dump ...
```

崩溃报告和日志仍然会生成。项目的测试里用的就是这个选项。
