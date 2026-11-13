# 在 VS Code 里编译、运行和调试

> 配合 [教程首页](README.md) 的“运行示例”和 [第 12 课：调试技巧](12-debugging.md) 使用。
>
> 这份说明是按 VS Code 和两个扩展的常规用法写的，**没有在 Mac 上逐步实测过**。菜单的名字和快捷键可能随版本略有不同，对不上的地方以你看到的界面为准，并告诉我们来修正。

## 1. 准备

**安装工具**（只做一次）：

| 系统 | 命令或步骤 |
|---|---|
| macOS | `xcode-select --install`，然后 `brew install cmake ninja` |
| Windows | 安装 Visual Studio 2022 社区版，勾选“使用 C++ 的桌面开发”（自带编译器、CMake、Ninja） |

**安装 VS Code 扩展**（左侧栏的“扩展”图标里搜索）：

| 扩展 | 作用 |
|---|---|
| **C/C++**（Microsoft） | 代码提示、跳转到定义；Windows 上的调试器 |
| **CMake Tools**（Microsoft） | 识别 CMake 项目，提供“编译”“运行”“调试”按钮 |
| **CodeLLDB**（Vadim Chugunov） | macOS 上的调试器。只在第 5 节用到，可以先不装 |

## 2. 打开示例项目

菜单 **文件 → 打开文件夹**，选择仓库里的 `docs/tutorial/cpp/examples` 这个文件夹。

注意要打开的是 **`examples` 这一层**，不是仓库的根目录。CMake Tools 会在打开的文件夹里找 `CMakeLists.txt`；根目录的那个是整个平台的，需要先安装 OpenCV 等依赖。

第一次打开时，CMake Tools 会询问用哪个配置：

- 它问“选择配置预设”时，选 **default**（就是 `CMakePresets.json` 里定义的那个）。
- 如果它问的是“选择工具包（Kit）”，说明没有识别到预设。按 `Cmd+Shift+P`（Windows 是 `Ctrl+Shift+P`）打开命令面板，输入 `CMake: Select Configure Preset`，选 **default**。

选好之后它会自动执行配置，相当于在终端里敲 `cmake --preset default`。底部的输出面板里看到 “Build files have been written to …” 就是成功了。

**Windows 上要留意用的是哪个编译器。** 配置时输出的第一行会写明，例如：

```
-- The CXX compiler identification is MSVC 19.44...      ← Visual Studio 的编译器
-- The CXX compiler identification is GNU 16.2.0         ← MinGW（Windows 上的 GCC）
```

两种都能编译教程的示例。区别是：

| | MSVC | MinGW |
|---|---|---|
| 是否经过自动构建验证 | 是，每次提交都验证 | **否**，没有验证过 |
| 调试器 | Visual Studio 的调试器（`cppvsdbg`） | GDB（`cppdbg`） |
| 和整个平台是否一致 | 一致，平台在 Windows 上用 MSVC | 不一致 |

建议用 MSVC。如果电脑上同时装了两个，从普通的终端启动 VS Code 时，CMake 常常会选中 MinGW。要换成 MSVC：

1. 关掉 VS Code。
2. 打开开始菜单里的“x64 Native Tools Command Prompt for VS 2022”。
3. 进入 `examples` 目录，删掉旧的构建目录（`rmdir /s /q build`），再输入 `code .`。

> Windows 上如果配置失败、提示找不到编译器：先打开“x64 Native Tools Command Prompt for VS 2022”，在里面进入 `examples` 目录，输入 `code .` 启动 VS Code。这样 VS Code 就带着编译器的环境变量。

## 3. 编译和运行

### 3.1 先找到“启动目标”在哪里

“启动目标”就是点运行或调试时要启动的那个示例。**新版本的 CMake Tools 默认不在状态栏上显示这些按钮了**，所以窗口最下面可能什么都看不到。有三个地方可以选，任选一个：

**办法一：命令面板（任何版本都能用，最可靠）**

1. 按 `Cmd+Shift+P`（Windows 是 `Ctrl+Shift+P`）。
2. 输入 `CMake: Set Launch/Debug Target`，回车。
3. 在弹出的列表里选一个示例。目标的名字是“l课号_文件名”，比如第 12 课的 `debug_me.cpp` 是 `l12_debug_me`。列表很长，可以直接输入 `l12` 过滤。

**办法二：左侧栏的 CMake 面板**

1. 点最左边一列图标里的 CMake 图标（一个三角形里套着三角形的图案；如果图标太多被收起来了，点那一列底部的 `…`）。
2. 面板上半部分是“**项目状态**”（Project Status），里面有几行：配置、生成（Build）、调试（Debug）、启动（Launch）。
3. 把鼠标移到“调试”或“启动”那一行，右边会出现一个铅笔图标，点它就可以选目标；出现的三角形或小虫子图标就是运行、调试。

面板下半部分是“项目大纲”，列出了所有目标。在某个目标上点右键，也可以直接“运行”“调试”或“设为启动/调试目标”。

**办法三：把按钮放回状态栏**

如果想像老版本那样在窗口最下面看到按钮：打开设置（`Cmd+,`），搜索 `cmake status bar visibility`，把“Cmake › Options: Status Bar Visibility”从 `hidden` 改成 `visible`。之后状态栏上会出现预设名、**Build**、小虫子、三角形，以及方括号里的当前目标；点方括号就能换目标。

### 3.2 编译和运行

选好启动目标之后：

| 操作 | 快捷键 | 命令面板里的名字 |
|---|---|---|
| 编译 | `F7` | `CMake: Build` |
| 运行（不调试） | `Shift+F5` | `CMake: Run Without Debugging` |
| 调试 | `Ctrl+F5` | `CMake: Debug` |

运行时它会先编译这个示例，再在下方的终端里启动它。

Mac 的笔记本键盘上，`F5`、`F7` 这些键默认是调亮度、音量的，要同时按住 `fn`。快捷键不起作用时，用命令面板输入右边那一列的名字，效果一样。

**只想编译某一个示例**（默认是编译全部，第一次会比较慢）：命令面板里 `CMake: Set Build Target`，选一个目标。运行和调试时它只会编译需要的那一个，所以一般不用管这一项。

## 4. 调试

### 4.1 最快的办法

1. 在编辑器里打开一个源文件，在某一行的**行号左边点一下**，出现一个红点：这就是**断点**。
2. 把这个示例选为启动目标（第 3.1 节）。
3. 按 `Ctrl+F5`，或者在命令面板里输入 `CMake: Debug`。

程序会运行到断点处停下，那一行被高亮。这时：

| 你想做的 | 操作 | 快捷键 |
|---|---|---|
| 执行这一行，停在下一行（不进入函数） | 单步跳过 | `F10` |
| 进入这一行调用的函数里面 | 单步进入 | `F11` |
| 把当前函数执行完，回到调用它的地方 | 单步跳出 | `Shift+F11` |
| 继续运行，直到下一个断点 | 继续 | `F5` |
| 结束调试 | 停止 | `Shift+F5` |

窗口上方会出现一条小工具栏，上面的按钮和这些快捷键一一对应。

### 4.2 看变量、看调用栈

调试时左侧栏会切换到“运行和调试”视图：

| 面板 | 内容 | 对应第 12 课的 |
|---|---|---|
| **变量** | 当前函数里所有局部变量的值。可以点开 `vector`、结构体看里面 | `frame variable` |
| **监视** | 自己添加想一直盯着的表达式，如 `rows * cols`、`queue.size()` | `print 表达式` |
| **调用堆栈** | 程序是怎样一层层调用到这里的。点某一层，可以看那一层的变量 | `bt` |
| **断点** | 所有断点的列表，可以临时关掉某一个 | `breakpoint list` |

把鼠标悬停在代码里的变量上，也会显示它的值。

### 4.3 条件断点

第 12 课里有一个用法：只有 `rows == 2` 时才停下来。

在红点上点右键 → **编辑断点** → 选“表达式”，输入 `rows == 2`，回车。红点会变成带一个等号的样子。之后只有条件成立时程序才会在这里停下。

第 12 课里的命令和这里的操作对照：

| LLDB 命令（第 12 课） | VS Code 里 |
|---|---|
| `breakpoint set --file debug_me.cpp --line 33` | 在第 33 行行号左边点一下 |
| `breakpoint set -f debug_me.cpp -l 33 -c 'rows == 2'` | 右键红点 → 编辑断点 → 表达式 `rows == 2` |
| `run` | `Ctrl+F5` |
| `next` | `F10` |
| `step` | `F11` |
| `finish` | `Shift+F11` |
| `continue` | `F5` |
| `frame variable` | 看“变量”面板 |
| `bt` | 看“调用堆栈”面板 |
| `thread backtrace all` | “调用堆栈”面板里列出了每个线程，逐个点开 |

学调试时，建议命令行和图形界面都试一次。命令行的好处是到了没有图形界面的机器上（比如产线上的设备）也能用。

### 4.4 建议的第一次练习

用第 12 课的 `debug_me.cpp`：

1. 把启动目标选为 `l12_debug_me`。
2. 按第 12 课“跟着做一遍”的步骤，在它说的那一行设置条件断点。
3. `Ctrl+F5` 开始，用 `F5`、`F10` 观察变量，找到那个 bug。

## 5. 需要命令行参数、或者从文件读输入时

上面“最快的办法”有一个限制：它不能给程序传命令行参数，也不能把文件接到标准输入。这时需要写一个**启动配置**。

在 `examples` 文件夹下新建 `.vscode/launch.json`（这个文件是你个人的设置，已经被 `.gitignore` 忽略，不会提交）：

**macOS**（需要安装 CodeLLDB 扩展）：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "调试当前启动目标",
      "type": "lldb",
      "request": "launch",
      "program": "${command:cmake.launchTargetPath}",
      "args": [],
      "cwd": "${workspaceFolder}"
    },
    {
      "name": "第 00 课 main：从 input.txt 读输入",
      "type": "lldb",
      "request": "launch",
      "program": "${workspaceFolder}/build/bin/l00_main",
      "cwd": "${workspaceFolder}",
      "stdio": ["${workspaceFolder}/lesson00/input.txt", null, null]
    }
  ]
}
```

**Windows**：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "调试当前启动目标",
      "type": "cppvsdbg",
      "request": "launch",
      "program": "${command:cmake.launchTargetPath}",
      "args": [],
      "cwd": "${workspaceFolder}",
      "console": "integratedTerminal"
    }
  ]
}
```

说明：

- `${command:cmake.launchTargetPath}` 的意思是“CMake Tools 里当前选中的启动目标”，并且会先编译它。所以换一个示例调试时不用改这个文件。
- 命令行参数写在 `"args"` 里，每个参数一个字符串，例如 `"args": ["recipe", "list"]`。
- 写好之后，在左侧栏的“运行和调试”视图顶部的下拉框里选这个配置，按 `F5` 开始（注意这里是 `F5`，不是 `Ctrl+F5`）。

## 6. 调试带内存检查、线程检查的示例

在 macOS 上，示例是带着 AddressSanitizer 或 ThreadSanitizer 编译的（第 13、15 课）。检查工具发现问题时，会把报告打印在下方的终端或“调试控制台”里，然后结束程序。

那些标了 `BUILD_ONLY` 的示例是故意有错的，用来看这种报告：把它选为启动目标，直接运行（`Shift+F5`），在终端里读报告。报告里的文件名和行号可以按住 `Cmd` 点击，直接跳到那一行。

## 7. 调试整个平台（vision-cli）

学到后面要调试项目本身时，方法相同，区别只有三点：

1. 打开的文件夹是**仓库的根目录**。
2. 要先按 [构建说明](../../dev/build.md) 装好依赖。配置预设 macOS 选 `default`，Windows 选 `windows`。
3. `vision-cli` 需要命令行参数，所以要用第 5 节的启动配置，把参数写进 `"args"`，例如：

```json
"args": ["--data", "vision-data", "recipe", "list"]
```

## 8. 常见问题

| 现象 | 原因和处理 |
|---|---|
| 状态栏上没有 CMake 的按钮 | 新版本默认不显示，见第 3.1 节的三个办法。如果命令面板里也搜不到 `CMake:` 开头的命令，说明打开的不是 `examples` 这一层，或者 CMake Tools 扩展没装好 |
| 配置时提示找不到 Ninja | macOS：`brew install ninja`。Windows：从“x64 Native Tools Command Prompt”里用 `code .` 启动 |
| 断点是灰色的空心圆，程序不停 | 这个源文件不属于当前的启动目标；或者程序还没重新编译。先按 `F7` |
| 单步时光标乱跳、有些变量看不到 | 这个示例打开了优化（第 14 课等测量时间的示例）。优化后的程序和源代码不再一一对应，这是正常现象 |
| macOS 第一次调试时弹出“开发者工具访问”的授权框 | 输入电脑密码允许，只需要一次 |
| Windows 终端里中文是乱码 | 在终端里先执行 `chcp 65001` |
| Windows 上想换编译器（MinGW ↔ MSVC） | 编译器是在第一次配置时定下来的，记在 `build` 目录里。删掉 `build` 目录后，按第 2 节的办法从对应的环境里重新打开 VS Code |
| 用 MinGW 时调试不起来 | 最快的调试方式默认找的是 Visual Studio 的调试器。MinGW 要用 GDB：`launch.json` 里把 `"type"` 写成 `"cppdbg"`，并加上 `"MIMode": "gdb"` |
| 改了代码，运行的还是旧结果 | 用 CMake Tools 的运行或调试会自动重新编译；如果是在终端里手动运行 `build/bin/` 下的程序，要先按 `F7` |
| 想从头再来 | 命令面板里 `CMake: Delete Cache and Reconfigure` |
