# 现代工程 C++ 教程

写给学过 C++、做过算法题，但没有接触过工程项目的同学。

## 这套教程是什么

竞赛 C++ 和工程 C++ 用的是同一门语言，但写法差别很大。竞赛代码只跑一次、只有自己看；工程代码要被别人读、被反复修改、连续运行很多年。

这套教程**不从头讲语法**，只讲“从竞赛写法到工程写法”需要补上的部分，并且**每一课都用本项目里的真实代码做例子**。学完之后，你应该能读懂 `core/` 下的全部代码，并且能独立写算子和领域对象。

## 怎么学

每一课的结构相同：

1. **为什么需要它**：先看一个不这样做会出什么问题的例子
2. **怎么做**：概念和写法
3. **项目里的例子**：打开真实的代码文件对照着看
4. **练习**：动手改代码或写代码，每课 2～4 题
5. **自测题**：答得出来就可以进入下一课

每课大约需要 1～2.5 小时。示例代码在 `examples/` 下，都可以直接编译运行。

## 课程大纲

| 课 | 主题 | 学完能做什么 | 项目里对应的代码 | 状态 |
|---|---|---|---|---|
| [00](00-from-contest-to-engineering.md) | 从竞赛 C++ 到工程 C++ | 知道两种写法差在哪里，为什么 | — | ✅ |
| [01](01-object-oriented-basics.md) | 面向对象基础：自己写一个字符串类（并用复数类做对照） | 明白封装、构造、析构、拷贝、移动各自解决什么问题；知道五法则和零法则 | `Roi`、`Image`、`Recipe` | ✅ |
| [02](02-project-organization.md) | 项目是怎么组织的：头文件、源文件、命名空间、CMake | 看懂项目目录；能新建一个模块并编译 | `core/kernel/`、各级 `CMakeLists.txt` | ✅ |
| [03](03-values-references-move.md) | 值、引用、`const`、移动语义 | 知道什么时候拷贝、什么时候不拷贝；看懂 `std::move` | `operator_catalog.cpp`（一个真实的 bug） | ✅ |
| [04](04-raii-smart-pointers.md) | 资源管理：RAII 与智能指针 | 不再手写 `new` / `delete`；会用 `unique_ptr`、`shared_ptr` | `Image`、`ExecutionService` | ✅ |
| [05](05-class-design.md) | 类的设计：封装、不变量、值对象 | 写出“创建不出非法状态”的类；分清值对象和实体 | `Roi`、`Recipe` | ✅ |
| [06](06-error-handling.md) | 错误处理：`Result`、`optional`、异常 | 知道三种方式各自什么时候用 | `result.h`、`Recipe::connect` | ✅ |
| [07](07-variant-and-bindings.md) | `variant`、`visit`、结构化绑定 | 看懂端口上流动的 `Value` | `value.h`、`describeValue` | ✅ |
| [08](08-interfaces-polymorphism.md) | 接口与多态：虚函数、依赖倒置 | 看懂“领域层定义接口，适配器实现接口” | `Operator`、`RecipeRepository`、`NodeRunner` | ✅ |
| [09](09-lambdas-algorithms.md) | lambda、`std::function` 与 STL 算法 | 用 `find_if`、`any_of` 代替手写循环 | `recipe.cpp`、`in_memory_event_bus.cpp` | ✅ |
| [10](10-templates-macros.md) | 模板入门与宏 | 看懂 `Result<T>`、`ctx.input<T>()` 和算子注册宏 | `result.h`、`VISION_REGISTER_OPERATOR` | ✅ |
| [11](11-unit-testing.md) | 单元测试：GoogleTest | 给自己写的算子和类写测试 | `tests/` | ✅ |
| [12](12-debugging.md) | 调试技巧 | 按固定的步骤定位问题；会用调试器的断点、单步、调用栈 | `vision-cli recipe debug` | ✅ |
| [13](13-memory-analysis.md) | 内存分析 | 用工具发现越界、泄漏、未定义行为；识别“没有泄漏但一直在涨” | `Image::create`、`planCache` | ✅ |
| [14](14-performance.md) | 性能分析 | 正确地测量时间；用性能分析器找到热点；知道最常见的几类性能问题 | 各节点的耗时记录 | ✅ |
| [14 延伸](14a-memory-pool.md) | 内存池与缓冲区复用 | 测量申请内存的开销；实现小块池和自动归还的缓冲区池；判断什么时候不需要池 | `Image` 的像素内存（目前未使用池） | ✅ |
| [15 预备](15a-threads-and-processes.md) | 线程和进程到底是什么（多图） | 从 CPU 和内存的角度理解进程、线程、上下文切换；知道线程共享什么、什么时候用线程或进程 | 项目的进程与线程结构 | ✅ |
| [15](15-concurrency-basics.md) | 并发基础：线程、互斥锁、条件变量 | 识别数据竞争；正确使用锁和条件变量 | `in_memory_event_bus.cpp`、`execution_service.cpp` | ✅ |
| [16](16-promise-future-inference.md) | `promise`、`future` 与生产者消费者：推理服务 | 实现一个多工位共用、按批推理的推理服务 | 推理后端（待实现）、热切换 | ✅ |
| [16 延伸](16a-thread-pool.md) | 线程池 | 实现一个最小的线程池；并行执行互不依赖的节点；避开四个常见的坑 | 节点并行执行（计划中） | ✅ |
| [17](17-logging-and-core-dumps.md) | 日志与崩溃转储 | 写出有用的日志；从 core 文件或 minidump 里找出崩溃原因 | `log.h`、`adapters/logging`、`adapters/crash` | ✅ |

学完第 00～04 课之后，可以同时开始 [OpenCV 与机器视觉基础教程](../opencv/README.md)。

## 建议的学习顺序

- **想尽快写算子**：00 → 01 → 02 → 03 → 06，然后看 [如何新增一个算子](../../dev/how-to-add-operator.md)。
- **想读懂领域层**：再学 04 → 05 → 07 → 08 → 09 → 10 → 11。
- **工程能力**：12（调试）→ 13（内存）→ 14（性能）→ 14 延伸（内存池）→ 17（日志与崩溃转储）。遇到问题时随时可以先看这几课。
- **多线程**：15 预备 → 15 → 16 → 16 延伸。实现推理和工位运行之前学。没有学过操作系统的话，预备课不要跳过。

## 运行示例：用 CMake（推荐）

全部示例是一个 CMake 项目，在 [`examples/`](examples/) 目录下。三条命令，三个系统上相同：

```bash
cd docs/tutorial/cpp/examples
```

```bash
cmake --preset default
```

```bash
cmake --build --preset default
```

```bash
ctest --preset default
```

第一条是配置，只需要做一次；第二条编译全部示例；第三条把它们都运行一遍并报告结果。

**只编译、运行某一个示例**。目标的名字是“l课号_文件名”，可执行程序都在 `build/bin/` 下：

```bash
cmake --build --preset default --target l03_registry_bug
```

```bash
./build/bin/l03_registry_bug
```

Windows 上是 `build\bin\l03_registry_bug.exe`。

**在编辑器里用**：用 VS Code（安装 CMake Tools 扩展）、CLion 或 Visual Studio 打开 `examples` 这个文件夹，它们会自动识别 CMake 项目，可以选择一个目标直接编译、运行、打断点调试（第 12 课）。VS Code 的详细步骤见 [在 VS Code 里编译、运行和调试](vscode-guide.md)。

几点说明：

- **内存检查和线程检查是自动加上的**：在 macOS 和 Linux 上，每个示例都按它的内容带着 AddressSanitizer 或 ThreadSanitizer 编译（第 13、15 课会讲），有问题时运行就会报告。Windows 上不加。
- **警告默认不当成错误**。换了更新的编译器、多出几条新警告时，示例仍然能编译。自动构建时会打开（`-DCPPTUT_WARNINGS_AS_ERRORS=ON`）。
- **标了 `BUILD_ONLY` 的示例是故意有错的**，用来演示这些工具的报告。它们会被编译，但 `ctest` 不运行它们；按对应课程里的说明手动运行 `build/bin/` 下的程序来看报告。
- **第 00、01 课还没有讲 CMake**（第 02 课才讲）。前两课照着上面的命令用就可以，不需要看懂 `CMakeLists.txt`；学完第 02 课再回来读它。

一条命令做完上面三步（自动构建用的就是它）：

| 系统 | 命令 |
|---|---|
| macOS / Linux | `bash docs/tutorial/cpp/examples/run_all.sh` |
| Windows（在“x64 Native Tools Command Prompt for VS 2022”里） | `docs\tutorial\cpp\examples\run_all.bat` |

## 环境

教程的示例只用到标准库，不需要安装 OpenCV 等依赖。完整项目的环境见 [构建说明](../../dev/build.md)。

| 系统 | 准备 |
|---|---|
| macOS | `xcode-select --install`；`brew install cmake ninja` |
| Windows | 安装 Visual Studio 2022 社区版（勾选“使用 C++ 的桌面开发”，自带 CMake 和 Ninja），打开“x64 Native Tools Command Prompt for VS 2022” |
| Linux | `sudo apt-get install g++ cmake ninja-build` |

## 不用 CMake，直接编译一个文件

每一课的“动手运行”里给出的是直接调用编译器的命令。它适合只有一个源文件的小例子，也能让你看清楚 CMake 替你做了什么：

| 系统 | 编译单个示例 |
|---|---|
| macOS | `clang++ -std=c++17 -Wall -Wextra 文件.cpp -o 程序名` |
| Windows | `cl /std:c++17 /W4 /EHsc /utf-8 文件.cpp /Fe:程序名.exe` |
| Linux | `g++ -std=c++17 -Wall -Wextra 文件.cpp -o 程序名` |

Windows 编译选项的含义：`/W4` 打开警告，`/EHsc` 启用异常，`/utf-8` 表示源文件是 UTF-8 编码（示例里有中文）。如果运行时中文显示成乱码，先执行 `chcp 65001` 把终端切换到 UTF-8。

所有示例都在自动构建里用 Windows（MSVC）和 Linux（GCC）编译并运行过。
