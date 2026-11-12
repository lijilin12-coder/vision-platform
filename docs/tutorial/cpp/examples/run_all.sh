#!/usr/bin/env bash
# 编译并运行全部教程示例（macOS / Linux）。自动构建也用这个脚本。
#
#   用法：在任意目录下执行   bash docs/tutorial/cpp/examples/run_all.sh
#   可以用环境变量 CXX 指定编译器，例如：CXX=clang++ bash run_all.sh
#
# 标了“只编译”的是【故意】有错的示例：它们用来演示内存检查、线程检查工具的报告，运行会报错。
set -euo pipefail

cd "$(dirname "$0")"
CXX="${CXX:-c++}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

WARN="-std=c++17 -Wall -Wextra -Wpedantic -Werror -g"
ASAN="$WARN -fsanitize=address,undefined"
TSAN="$WARN -pthread -fsanitize=thread"

# run <目录> <编译选项> <源文件...>      编译并运行
run() {
    local dir="$1" flags="$2"; shift 2
    echo "== $dir: $* "
    (cd "$dir" && $CXX $flags "$@" -o "$OUT/prog" && "$OUT/prog" > "$OUT/log" 2>&1) || { cat "$OUT/log" 2>/dev/null; echo "失败：$dir $*"; exit 1; }
    tail -1 "$OUT/log"
}
# build_only <目录> <编译选项> <源文件...>   只编译
build_only() {
    local dir="$1" flags="$2"; shift 2
    echo "== $dir: $*（只编译）"
    (cd "$dir" && $CXX $flags "$@" -o "$OUT/prog") || { echo "失败：$dir $*"; exit 1; }
}

run lesson00 "$ASAN" test.cpp blob_count.cpp
echo "== lesson00: main.cpp"
(cd lesson00 && $CXX $ASAN main.cpp blob_count.cpp -o "$OUT/prog" && test "$("$OUT/prog" < input.txt)" = "2") || { echo "失败：lesson00 main"; exit 1; }

run lesson01 "$ASAN" test.cpp my_string.cpp
run lesson01 "$ASAN" complex_rule_of_zero.cpp
run lesson01 "$ASAN" c_style.cpp
build_only lesson01 "$ASAN" broken_copy.cpp

echo "== lesson02: CMake 项目"
cmake -S lesson02 -B "$OUT/lesson02" -G Ninja -DCMAKE_COMPILE_WARNING_AS_ERROR=ON > /dev/null
cmake --build "$OUT/lesson02" > /dev/null
ctest --test-dir "$OUT/lesson02" --output-on-failure > /dev/null
"$OUT/lesson02/app/overlap_app"

run lesson03 "$ASAN" copy_counter.cpp
run lesson03 "$ASAN" registry_bug.cpp
build_only lesson03 "$ASAN" dangling.cpp

run lesson04 "$ASAN" raii.cpp
run lesson05 "$ASAN" measurement.cpp
run lesson06 "$ASAN -I../../../../../core/common/include" errors.cpp
run lesson07 "$ASAN" variant.cpp
run lesson08 "$ASAN" interfaces.cpp
run lesson09 "$ASAN" algorithms.cpp
run lesson10 "$ASAN" templates.cpp

run lesson12 "$ASAN" debug_me.cpp            # 结果故意是错的，但程序正常结束

build_only lesson13 "$ASAN" overflow.cpp
build_only lesson13 "$ASAN" leak.cpp
build_only lesson13 "$WARN -fsanitize=undefined" int_overflow.cpp
run lesson13 "$ASAN" leak_fixed.cpp
run lesson13 "$ASAN" growth.cpp

run lesson14 "-std=c++17 -Wall -Wextra -Wpedantic -Werror -O2" benchmark.cpp

run lesson15a "$TSAN" address_space.cpp
run lesson15a "$TSAN" context_switch.cpp
run lesson15a "-std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -pthread" why_threads.cpp

build_only lesson15 "$TSAN" race.cpp
run lesson15 "$TSAN" sync.cpp
run lesson16 "$TSAN" future_basics.cpp
run lesson16 "$TSAN" inference_worker.cpp
run lesson16 "$ASAN -pthread" inference_worker.cpp

build_only lesson17 "$WARN -O0" crash.cpp

echo
echo "全部示例通过"
