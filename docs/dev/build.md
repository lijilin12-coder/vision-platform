# 构建与运行

## 依赖

| 依赖 | 版本 | 用途 |
|---|---|---|
| CMake | ≥ 3.20 | 构建 |
| C++ 编译器 | 支持 C++17（GCC ≥ 9、Clang ≥ 10、AppleClang ≥ 12） | |
| OpenCV | 4.x（core、imgproc、imgcodecs） | 只有算子和适配器使用 |
| nlohmann-json | 3.x | JSON 仓储 |
| GoogleTest | 1.10+ | 测试 |
| Ninja | 可选 | 更快的构建 |

### macOS（开发机）

```bash
brew install cmake ninja opencv nlohmann-json googletest
```

### Ubuntu / Debian

```bash
sudo apt-get install -y cmake ninja-build g++ libopencv-dev nlohmann-json3-dev libgtest-dev
```

## 构建与测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure
```

测试包括：

| 测试 | 内容 | 是否链接 OpenCV |
|---|---|---|
| `kernel_tests` | 共享内核的值对象 | 否 |
| `catalog_tests` | 算子注册规则、主版本号检查 | 否 |
| `authoring_domain_tests` | 方案编排的不变量 R1～R6、发布规则 P1/P5 | 否 |
| `execution_domain_tests` | 执行计划、检测的不变量 I1～I4、判定 | 否 |
| `integration_tests` | 真实算子 + OpenCV + JSON 仓储的完整流程 | 是 |
| `boundary_violation_fails_to_compile` | 跨上下文引用领域类必须编译失败 | — |

## 命令行示例

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
