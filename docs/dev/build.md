# 构建与运行

开发机可以是 **macOS** 或 **Windows**，两者都必须能编译并通过全部测试。Linux 也可以构建。

## 依赖

| 依赖 | 版本 | 用途 |
|---|---|---|
| CMake | ≥ 3.21 | 构建（使用预设 `CMakePresets.json`） |
| Ninja | 任意 | 构建工具 |
| C++ 编译器 | 支持 C++17：AppleClang ≥ 12、MSVC 2022、GCC ≥ 9 | |
| OpenCV | 4.x（core、imgproc、imgcodecs） | 只有算子和适配器使用 |
| nlohmann-json | 3.x | JSON 仓储 |
| GoogleTest | 1.10+ | 测试 |

### ONNX Runtime（自动下载）

运行模型用的推理引擎。不需要自己安装：第一次运行 `cmake` 配置时会自动下载微软官方的预编译库（约 120 MB，来自 nuget.org），之后用缓存。

- 下载不了时：手工下载 `cmake/OnnxRuntime.cmake` 里写的那个地址，把文件改名为 `.zip` 解压，配置时加 `-DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME_NUGET=<解压出来的目录>`
- 暂时不需要模型功能：配置时加 `-DVISION_WITH_ONNXRUNTIME=OFF`。其余功能不受影响；用到模型的方案会提示“没有配置推理引擎”

## macOS

```bash
xcode-select --install
```

```bash
brew install cmake ninja opencv nlohmann-json googletest
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

## Windows

### 一次性准备

1. 安装 **Visual Studio 2022 社区版**，勾选“使用 C++ 的桌面开发”。它自带 CMake 和 Ninja。
2. 安装 **vcpkg**（管理 C++ 依赖），并设置环境变量 `VCPKG_ROOT`。在 PowerShell 中：

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
```

```powershell
C:\vcpkg\bootstrap-vcpkg.bat
```

```powershell
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg", "User")
```

设置环境变量后，需要重新打开终端才生效。

### 编译与测试

从开始菜单打开 **“x64 Native Tools Command Prompt for VS 2022”**（这个终端里已经配置好了编译器），进入项目目录：

```bat
cmake --preset windows
```

```bat
cmake --build --preset windows
```

```bat
ctest --preset windows
```

第一次运行 `cmake --preset windows` 时，vcpkg 会根据仓库里的 `vcpkg.json` 自动下载并编译 OpenCV 等依赖，大约需要 20～40 分钟。之后会直接使用缓存。

也可以用 Visual Studio 直接打开项目文件夹（“打开本地文件夹”），它会识别 `CMakePresets.json`，选择 `windows` 预设即可编译和调试。

### Windows 上需要 Linux 的工具

鸿蒙版 OpenCV 的交叉编译和模型转换工具需要 Ubuntu 22.04。在 Windows 上用 WSL2：

```powershell
wsl --install -d Ubuntu-22.04
```

## Linux（Ubuntu / Debian）

```bash
sudo apt-get install -y cmake ninja-build g++ libopencv-dev nlohmann-json3-dev libgtest-dev
```

然后和 macOS 一样使用 `default` 预设。

## 自动构建

每个 PR 和 `main` 分支上的每次推送，GitHub Actions 都会在 **Windows（MSVC）** 和 **Linux（GCC）** 上编译并运行全部测试、命令行冒烟测试和教程示例（见 `.github/workflows/ci.yml`）。只修改 Markdown 文档时不会触发。

macOS 不在自动构建里：日常开发就在 Mac 上进行；macOS 构建机在私有仓库中按 10 倍计费。

## 跨平台编码约定

两个人在不同的系统上开发，下面几条能避免“在我这里可以，在你那里不行”：

| 约定 | 原因 |
|---|---|
| 源文件一律保存为 **UTF-8** | MSVC 用 `/utf-8` 选项编译；否则中文字符串会乱码 |
| 路径和 `std::string` 之间的转换只用 `vision/common/path.h` 里的 `pathFromUtf8` / `pathToUtf8` | Windows 上直接用 `std::string` 构造路径会按本地编码（GBK）解释，中文路径会出错 |
| 读写图像用 `imaging::loadImage` / `saveImage`，不要直接用 `cv::imread` / `cv::imwrite` | 后者在 Windows 上不支持中文路径 |
| 不要用全大写的普通单词做标识符（如 `ERROR`、`min`、`max`、`near`、`far`） | Windows 的系统头文件把它们定义成了宏 |
| 不要用 `<bits/stdc++.h>` 和编译器特有的扩展 | 只有 GCC 有 |
| 提交前在本机跑一遍测试；另一个平台由自动构建检查 | |

## 测试说明

| 测试 | 内容 | 是否链接 OpenCV |
|---|---|---|
| `kernel_tests` | 共享内核的值对象 | 否 |
| `catalog_tests` | 算子注册规则、主版本号检查 | 否 |
| `authoring_domain_tests` | 方案编排的不变量 R1～R6、发布规则 P1/P5 | 否 |
| `execution_domain_tests` | 执行计划、检测的不变量 I1～I4、判定 | 否 |
| `integration_tests` | 真实算子 + OpenCV + JSON 仓储的完整流程；中文路径 | 是 |
| `logging_tests` | 日志的格式、级别过滤、文件轮转、多线程写入 | 否 |
| `crash_leaves_report_*` | 故意让程序以三种方式崩溃，检查崩溃报告、转储文件和日志是否留下 | — |
| `boundary_violation_fails_to_compile` | 跨上下文引用领域类必须编译失败 | — |

## 命令行示例

下面是 macOS / Linux 的写法。Windows 上把 `./build/apps/vision-cli` 换成 `build\apps\vision-cli.exe`，变量赋值的写法也不同，建议在 Git Bash 或 PowerShell 中运行。

```bash
CLI=./build/apps/vision-cli
$CLI sample-images samples/images                          # 生成 4 张示例图（2 张良品、2 张带亮斑）
RID=$($CLI recipe build samples/recipes/bright-spot.draft.json)   # 搭方案，输出方案 ID
$CLI recipe show $RID
$CLI recipe debug $RID samples/images/ng_01_one_spot.png   # 调试运行，查看每个节点的输出
VID=$($CLI recipe publish $RID)                            # 发布，输出方案版本 ID
$CLI version show $VID                                     # 以发布语言（JSON）查看方案版本
$CLI --events inspect $VID samples/images                  # 检测整个目录，--events 显示领域事件
```

数据默认保存在 `./vision-data/`，可以用 `--data <目录>` 指定。
