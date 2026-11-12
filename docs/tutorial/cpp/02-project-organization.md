# 第 2 课：项目是怎么组织的——头文件、源文件、命名空间、CMake

> 预计用时：2 小时
> 示例代码：[`examples/lesson02/`](examples/lesson02/)

## 这一课要回答的问题

竞赛里，一个程序就是一个 `.cpp` 文件，一条 `g++ a.cpp` 就编译完了。本项目有 50 多个源文件和头文件，分在十几个目录里。这一课回答三个问题：

1. 为什么要把代码拆成头文件（`.h`）和源文件（`.cpp`）？
2. 这么多文件，名字冲突了怎么办？
3. 它们是怎么被编译成一个程序的？

学完之后，你应该能看懂本项目的目录结构，能自己新建一个模块并让它编译通过。

## 1. 从一个文件到多个文件

### 一个文件为什么不够

假设整个项目写在一个文件里：

- **改一行，全部重新编译。** 本项目完整编译一次要一两分钟，如果每次改动都要等这么久，没法工作。
- **两个人没法同时改。** 你和你哥哥同时修改同一个文件，合并时到处是冲突。
- **找不到东西。** 几千行的文件，没人知道某个函数在哪。
- **没法复用。** 命令行工具和测试程序都要用同一套算法，只能复制粘贴。

### 编译的三个步骤

理解拆分方式之前，要先知道编译器是怎么工作的：

```
rect.cpp ──预处理──► 把 #include 的内容原样粘贴进来 ──编译──► rect.o ─┐
overlap.cpp ──预处理──► ……                          ──编译──► overlap.o ─┼─链接──► 程序
main.cpp ──预处理──► ……                             ──编译──► main.o ─┘
```

1. **预处理**：`#include "x.h"` 的意思非常朴素，就是**把 `x.h` 的全部内容复制粘贴到这一行**。
2. **编译**：每个 `.cpp` 文件**单独**编译成一个目标文件（`.o`，Windows 上是 `.obj`）。编译 `main.cpp` 时，编译器完全看不到 `rect.cpp` 里写了什么。
3. **链接**：把所有目标文件拼成一个程序，把“这里调用了 `area`”和“`area` 的实现在那里”对上。

关键在第 2 步：**每个源文件是单独编译的。** `main.cpp` 要调用 `area()`，编译器至少得知道 `area` 接收什么参数、返回什么类型。这个信息就放在头文件里。

### 声明和定义

| | 是什么 | 例子 | 放在哪 |
|---|---|---|---|
| **声明** | 告诉编译器“有这么个东西，长这样” | `double area(const Rect& rect);` | 头文件 `.h` |
| **定义** | 真正的实现 | `double area(const Rect& rect) { return ...; }` | 源文件 `.cpp` |

声明可以出现很多次，**定义在整个程序里只能有一个**。如果把函数的定义写在头文件里，而这个头文件被两个源文件包含，那么两个目标文件里各有一份定义，链接时就会报“重复定义”。

所以分工是：

- **头文件**是给使用者看的“说明书”：有哪些类型、哪些函数。
- **源文件**是实现：怎么做的。

这样还带来一个好处：只改了 `rect.cpp` 里的实现、没有改头文件时，**只需要重新编译 `rect.cpp` 这一个文件**，其他文件都不用动。

### `#pragma once`

```cpp
#pragma once
```

每个头文件的第一行都是它，作用是：同一个源文件里，这个头文件最多被粘贴一次。没有它的话，`a.h` 包含了 `rect.h`，`main.cpp` 又同时包含 `a.h` 和 `rect.h`，`Rect` 就会被定义两次，编译失败。

### 可以写在头文件里的定义

有几种定义允许（或必须）写在头文件里：

| 种类 | 例子 | 说明 |
|---|---|---|
| 类和结构体的定义 | `struct Rect { ... };` | 使用者需要知道它的大小和成员 |
| 类内部直接写的成员函数 | `double x() const { return x_; }` | 自动是 `inline` 的 |
| 标了 `inline` 的函数 | `inline double clampToZero(double v) { ... }` | `inline` 的意思是“允许多个源文件里各有一份相同的定义” |
| 模板 | `template <typename T> class Result { ... };` | 第 10 课讲 |

## 2. 示例项目

[`examples/lesson02/`](examples/lesson02/) 是一个结构完整的小项目，计算两个矩形的**重叠度**（交并比）。这个量在视觉里很常用，本项目的一致性标准里“缺陷位置重叠度 ≥ 0.9”说的就是它。

```
lesson02/
├── CMakeLists.txt                    # 项目的总配置
├── geometry/                         # 一个库
│   ├── CMakeLists.txt
│   ├── include/geometry/rect.h       # 对外的头文件：使用者能看到的
│   └── src/
│       ├── detail.h                  # 内部头文件：只有库自己能用
│       ├── rect.cpp
│       └── overlap.cpp
├── app/                              # 一个程序，使用 geometry 库
│   ├── CMakeLists.txt
│   └── main.cpp
└── tests/                            # 测试程序，也使用 geometry 库
    ├── CMakeLists.txt
    └── test.cpp
```

注意两点：

- **对外的头文件和内部的文件分开放。** `include/` 下是给别人用的，`src/` 下是自己的实现细节。
- **头文件放在 `include/geometry/` 而不是直接放在 `include/`。** 这样使用者写的是 `#include "geometry/rect.h"`，一眼看出它来自哪个库，也不会和别的库里同名的 `rect.h` 冲突。

本项目的每个模块都是这个结构，比如 `core/kernel/include/vision/kernel/image.h`。

## 3. 命名空间

### 解决什么问题

项目大了，名字一定会撞。你写了一个 `area` 函数，OpenCV 里可能也有一个 `area`。命名空间给名字加上“姓”：

```cpp
namespace geometry {

double area(const Rect& rect);

}  // namespace geometry
```

使用时写全名 `geometry::area(...)`。在同一个命名空间内部互相调用时，不用写前缀。

本项目的命名空间和目录、限界上下文一一对应：

| 命名空间 | 内容 |
|---|---|
| `vision` | 共享内核和通用组件 |
| `vision::catalog` | 算子目录上下文 |
| `vision::authoring` | 方案编排上下文 |
| `vision::execution` | 检测执行上下文 |
| `vision::operators` | 算子实现 |

`namespace vision::authoring { ... }` 是 C++17 的简写，等价于两层嵌套。

### 匿名命名空间：只在本文件内可见

```cpp
namespace {  // 没有名字

double unionArea(const Rect& a, const Rect& b) { ... }

}  // namespace
```

放在匿名命名空间里的函数和变量，**只在当前这个 `.cpp` 文件里可见**。别的文件里可以有同名的函数，互不影响。

规则很简单：**只在一个源文件里用到的辅助函数，一律放进匿名命名空间。** 打开 [`core/contexts/authoring/domain/src/recipe.cpp`](../../../core/contexts/authoring/domain/src/recipe.cpp)，开头的 `reject` 函数就是这样做的。

### 两条禁令

- **头文件里绝对不要写 `using namespace xxx;`。** 头文件会被粘贴到所有包含它的文件里，等于替所有使用者打开了这个命名空间。
- **源文件里也尽量不写 `using namespace std;`。** 可以对个别名字写 `using geometry::Rect;`，范围小、意图明确。

## 4. CMake：把文件变成程序

### CMake 是什么

文件一多，手写编译命令就不现实了。更麻烦的是，Mac、Windows、Linux 的编译器命令各不相同。**CMake 让你只描述一次“项目由哪些部分组成、谁依赖谁”，由它生成各个平台上的编译命令。**

CMake 里最重要的概念是**目标**（target）：一个目标就是一个要生成的东西，要么是一个库，要么是一个可执行程序。

### 读懂示例里的 CMake

**总配置** [`CMakeLists.txt`](examples/lesson02/CMakeLists.txt)：

```cmake
cmake_minimum_required(VERSION 3.20)
project(lesson02 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)            # 使用 C++17
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)         # 不使用编译器的私有扩展

if(MSVC)
  add_compile_options(/W4 /utf-8)     # 微软编译器的写法
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

enable_testing()

add_subdirectory(geometry)            # 进入子目录，读取那里的 CMakeLists.txt
add_subdirectory(app)
add_subdirectory(tests)
```

**库** [`geometry/CMakeLists.txt`](examples/lesson02/geometry/CMakeLists.txt)：

```cmake
add_library(geometry STATIC
  src/rect.cpp
  src/overlap.cpp)

target_include_directories(geometry
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
```

`add_library` 定义了一个名叫 `geometry` 的库目标。`target_include_directories` 告诉编译器到哪些目录里找头文件。

**程序** [`app/CMakeLists.txt`](examples/lesson02/app/CMakeLists.txt)：

```cmake
add_executable(overlap_app main.cpp)
target_link_libraries(overlap_app PRIVATE geometry)
```

`target_link_libraries` 表示 `overlap_app` 依赖 `geometry`。只这一行就够了，不需要再告诉它头文件在哪。原因见下一节。

### `PUBLIC` 和 `PRIVATE`：这一课最重要的部分

```cmake
target_include_directories(geometry
  PUBLIC  .../include      # 库自己用，并且“传递”给所有依赖 geometry 的目标
  PRIVATE .../src)         # 只有库自己用
```

| | 库自己能用 | 依赖它的目标能用 |
|---|---|---|
| `PUBLIC` | ✅ | ✅ |
| `PRIVATE` | ✅ | ❌ |

所以：

- `overlap_app` 链接了 `geometry`，自动获得了 `include/` 目录，可以写 `#include "geometry/rect.h"`。
- `overlap_app` **拿不到** `src/` 目录。如果它写 `#include "detail.h"`，编译会失败：找不到这个文件。

你可以亲手验证：把 [`app/main.cpp`](examples/lesson02/app/main.cpp) 里那行被注释掉的 `#include "detail.h"` 打开，重新编译，看看报什么错。

**这意味着“哪些东西对外公开”不是靠自觉，而是由构建系统强制的。** 库的作者可以放心修改 `src/` 下的任何东西，因为可以确定没有外人在用。

### 本项目正是用这个机制守住了架构边界

打开 [`cmake/VisionBoundaries.cmake`](../../../cmake/VisionBoundaries.cmake)：

```cmake
target_link_libraries(${target}
  PUBLIC vision_kernel vision_common
  PRIVATE ${ctx}_domain)
```

每个限界上下文的应用层**私有地**（`PRIVATE`）依赖自己的领域层。于是领域层的头文件目录不会传递出去。方案编排的代码如果想包含检测执行的领域头文件，结果和上面的 `detail.h` 一样：**编译失败**。

设计文档里说的“上下文边界由构建系统强制”，全部的技术手段就是你刚学的这个 `PRIVATE`。

## 5. 动手运行

在 `docs/tutorial/cpp/examples/lesson02` 目录下。

**macOS / Linux**（需要先安装 CMake 和 Ninja，见[构建说明](../../dev/build.md)）：

```bash
cmake -S . -B build -G Ninja      # 配置：读取 CMakeLists.txt，生成编译命令
cmake --build build               # 编译
ctest --test-dir build            # 运行测试
./build/app/overlap_app           # 运行程序，输出：重叠度：0.746725
```

**Windows**（在“x64 Native Tools Command Prompt for VS 2022”里）：

```bat
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
build\app\overlap_app.exe
```

四条命令在两个系统上几乎一样，这就是 CMake 的价值。

**试一试增量编译**：改一下 `geometry/src/rect.cpp`（比如加一行注释），再运行 `cmake --build build`。观察输出：只重新编译了 `rect.cpp`，然后重新链接，其他文件都没有动。

## 6. 两种最常见的链接错误

以后你一定会遇到这两种错误，提前认识一下。

### “未定义的引用”

```
undefined reference to `geometry::area(geometry::Rect const&)'        （GCC / Clang）
unresolved external symbol "double __cdecl geometry::area(...)"        （MSVC）
```

意思是：**有声明、有调用，但链接时找不到定义。** 常见原因：

| 原因 | 检查 |
|---|---|
| 新建了 `.cpp` 文件，但没有加进 `add_library(...)` 的列表 | 看 `CMakeLists.txt` |
| 忘了写 `target_link_libraries` | 这个目标依赖的库都链接了吗 |
| 头文件里声明了，源文件里忘了实现，或者参数类型写得不一样 | 对比声明和定义 |
| 定义时忘了写命名空间 | `.cpp` 里的函数是不是在 `namespace geometry { }` 里面 |

### “重复定义”

```
multiple definition of `geometry::detail::clampToZero(double)'         （GCC / Clang）
... already defined in rect.obj                                        （MSVC）
```

意思是：**同一个东西有两份定义。** 几乎总是因为在头文件里写了函数的实现，却没有加 `inline`。你可以把 [`geometry/src/detail.h`](examples/lesson02/geometry/src/detail.h) 里的 `inline` 删掉，重新编译，亲眼看一下这个错误。

## 7. 项目里的例子

| 文件 | 看什么 |
|---|---|
| [`CMakeLists.txt`](../../../CMakeLists.txt)（仓库根目录） | 和示例的总配置对照：多了 `find_package`（查找 OpenCV 等外部库）和 `include(cmake/VisionBoundaries.cmake)` |
| [`core/kernel/CMakeLists.txt`](../../../core/kernel/CMakeLists.txt) | 只有三行，和示例的 `geometry` 库几乎一样 |
| [`core/kernel/include/vision/kernel/geometry.h`](../../../core/kernel/include/vision/kernel/geometry.h) 和 [`core/kernel/src/kernel.cpp`](../../../core/kernel/src/kernel.cpp) | `Roi::rect` 在头文件里只有声明，实现在源文件里 |
| [`operators/CMakeLists.txt`](../../../operators/CMakeLists.txt) | 用 `file(GLOB ...)` 自动收集 `src/` 下所有 `.cpp`，所以新增算子不用改它 |
| [`core/contexts/authoring/`](../../../core/contexts/authoring/) | `domain/include`、`domain/src`、`application/include`、`application/src`：就是示例里的结构，重复了两层 |

## 8. 练习

1. **运行并验证 `PRIVATE`**：按第 5 节编译运行。然后打开 `app/main.cpp` 里的 `#include "detail.h"`，重新编译，记下报错信息，再改回去。
2. **制造两种链接错误**：
   - 把 `geometry/CMakeLists.txt` 里的 `src/overlap.cpp` 那一行删掉，重新编译，看“未定义的引用”。改回去。
   - 把 `detail.h` 里的 `inline` 删掉，重新编译，看“重复定义”。改回去。
3. **加一个函数**：给 geometry 库增加 `bool contains(const Rect& outer, const Rect& inner);`，判断 `inner` 是否完全在 `outer` 里面。要求：声明放在 `rect.h`，实现放在一个**新文件** `src/contains.cpp` 里，并在 `tests/test.cpp` 里补上测试。提示：别忘了改 `CMakeLists.txt`。
4. **读项目代码**：打开仓库根目录的 `CMakeLists.txt` 和 `cmake/VisionBoundaries.cmake`。`vision_domain_library` 这个函数给每个领域层目标链接了哪两个库？为什么只有这两个？

## 9. 自测题

1. `#include "rect.h"` 这一行，预处理器具体做了什么？
2. 声明和定义有什么区别？各放在哪里？为什么？
3. 只修改了 `rect.cpp` 里的实现，没有改任何头文件，需要重新编译哪些文件？
4. 一个只在 `overlap.cpp` 里用到的辅助函数，应该怎么写才不会和别的文件里的同名函数冲突？
5. `target_include_directories` 里的 `PUBLIC` 和 `PRIVATE` 有什么区别？
6. 看到“undefined reference”，你会先检查哪几件事？
7. 本项目是用什么手段保证“方案编排的代码不能使用检测执行的领域类”的？

## 下一课

[第 3 课：值、引用、`const` 和移动语义](03-values-references-move.md)：什么时候会发生拷贝，怎么避免不必要的拷贝，以及本项目里一个由 `std::move` 引起的真实 bug。
