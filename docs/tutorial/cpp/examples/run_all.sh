#!/usr/bin/env bash
# 编译并运行全部教程示例（macOS / Linux）。自动构建也用这个脚本。
#
#   用法：在任意目录下执行   bash docs/tutorial/cpp/examples/run_all.sh
#   可以用环境变量 CXX 指定编译器，例如：CXX=clang++ bash run_all.sh
#
# 它只是依次执行三条 CMake 命令（见 CMakeLists.txt 开头的说明）。
# 构建结果放在本目录的 build/ 下，可执行程序在 build/bin/。
set -euo pipefail
cd "$(dirname "$0")"

cmake --preset default
cmake --build --preset default
ctest --preset default

echo
echo "全部示例通过"
