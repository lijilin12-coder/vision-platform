# 现代工程 C++ 教程的全部示例。只用到标准库，不需要安装 OpenCV 等依赖。
#
#   配置：        cmake --preset default
#   编译全部：    cmake --build --preset default
#   运行全部：    ctest --preset default
#   只编译一个：  cmake --build --preset default --target l03_registry_bug
#   运行一个：    build/bin/l03_registry_bug        （Windows：build\bin\l03_registry_bug.exe）
#
# 目标的名字是 “l课号_文件名”。标了 BUILD_ONLY 的示例是【故意】有错的：它们用来演示内存检查、
# 线程检查工具的报告，只编译，不作为测试运行；想看报告就手动运行它。
cmake_minimum_required(VERSION 3.21)
project(cpp_tutorial LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)   # 默认带调试信息、不优化：方便用调试器
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")   # 所有可执行程序放在同一个目录，好找

enable_testing()
find_package(Threads REQUIRED)

if(MSVC)
  # 个别示例要打开优化（/O2）。MSVC 的调试配置默认带 /RTC1，它和 /O2 不能同时使用，所以去掉。
  string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  set(CPPTUT_WARNINGS /W4 /utf-8 /EHsc)
else()
  set(CPPTUT_WARNINGS -Wall -Wextra -Wpedantic)
endif()

# 把警告当成错误。自动构建时打开（run_all 脚本会打开它）；平时默认关闭，
# 这样换了更新的编译器、多出几条新警告时，示例仍然能编译。
option(CPPTUT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(CPPTUT_WARNINGS_AS_ERRORS AND NOT MSVC)
  list(APPEND CPPTUT_WARNINGS -Werror)
endif()

# 内存检查、线程检查工具只在 macOS 和 Linux 上启用。
# Windows 上不启用：MSVC 的用法不同；MinGW（Windows 上的 GCC）不带这些工具，加了会链接失败。
if(UNIX)
  set(CPPTUT_SANITIZERS ON)
else()
  set(CPPTUT_SANITIZERS OFF)
endif()

# cpptut_example(<课号> <名字> SOURCES <源文件...>
#                [SANITIZE address|thread|undefined|none]   用哪种检查工具编译（macOS、Linux 上有效；默认 address）
#                [OPTIMIZE]                                 打开优化（测量时间的示例需要）
#                [BUILD_ONLY]                               只编译，不作为测试运行
#                [SUFFIX <后缀>]                            同一个源文件用两种检查工具各编译一次时，用来区分目标名
#                [INCLUDE <目录...>])                       额外的头文件目录
function(cpptut_example lesson name)
  cmake_parse_arguments(A "OPTIMIZE;BUILD_ONLY" "SANITIZE;SUFFIX" "SOURCES;INCLUDE" ${ARGN})
  if(NOT A_SANITIZE)
    set(A_SANITIZE address)
  endif()
  set(target "l${lesson}_${name}${A_SUFFIX}")
  list(TRANSFORM A_SOURCES PREPEND "lesson${lesson}/")
  add_executable(${target} ${A_SOURCES})
  target_compile_options(${target} PRIVATE ${CPPTUT_WARNINGS})
  target_link_libraries(${target} PRIVATE Threads::Threads)
  if(A_INCLUDE)
    target_include_directories(${target} PRIVATE ${A_INCLUDE})
  endif()

  if(A_OPTIMIZE)
    target_compile_options(${target} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>)
  endif()

  # 内存检查、线程检查工具。优化过的测量示例不加（会让时间失真）。
  if(CPPTUT_SANITIZERS AND NOT A_OPTIMIZE AND NOT A_SANITIZE STREQUAL "none")
    if(A_SANITIZE STREQUAL "address")
      set(flags -fsanitize=address,undefined)
    else()
      set(flags -fsanitize=${A_SANITIZE})
    endif()
    target_compile_options(${target} PRIVATE ${flags})
    target_link_options(${target} PRIVATE ${flags})
  endif()

  if(NOT A_BUILD_ONLY)
    add_test(NAME ${target} COMMAND ${target})
    if(A_SANITIZE STREQUAL "thread")
      # 屏蔽标准库里一处已知的误报，原因见 tsan_suppressions.txt
      set_tests_properties(${target} PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan_suppressions.txt")
    endif()
  endif()
endfunction()

# ---- 第 00 课：从竞赛写法到工程写法 ----
cpptut_example(00 test SOURCES test.cpp blob_count.cpp)
cpptut_example(00 main SOURCES main.cpp blob_count.cpp BUILD_ONLY)
# main 从标准输入读数据：把 input.txt 喂给它，输出应该是 2
add_test(NAME l00_main COMMAND ${CMAKE_COMMAND}
  -DPROGRAM=$<TARGET_FILE:l00_main> -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/lesson00/input.txt -DEXPECTED=2
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_stdin.cmake)

# ---- 第 01 课：面向对象基础 ----
cpptut_example(01 test SOURCES test.cpp my_string.cpp)
cpptut_example(01 complex_rule_of_zero SOURCES complex_rule_of_zero.cpp)
cpptut_example(01 c_style SOURCES c_style.cpp)
cpptut_example(01 broken_copy SOURCES broken_copy.cpp BUILD_ONLY)

# ---- 第 02 课：项目的组织。它本身就是一个完整的小 CMake 项目，直接整个包含进来 ----
add_subdirectory(lesson02)
add_test(NAME l02_overlap_app COMMAND overlap_app)

# ---- 第 03～10 课 ----
cpptut_example(03 copy_counter SOURCES copy_counter.cpp)
cpptut_example(03 registry_bug SOURCES registry_bug.cpp)
cpptut_example(03 dangling SOURCES dangling.cpp BUILD_ONLY)
cpptut_example(04 raii SOURCES raii.cpp)
cpptut_example(05 measurement SOURCES measurement.cpp)
cpptut_example(06 errors SOURCES errors.cpp INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/common/include")
cpptut_example(07 variant SOURCES variant.cpp)
cpptut_example(08 interfaces SOURCES interfaces.cpp)
cpptut_example(09 algorithms SOURCES algorithms.cpp)
cpptut_example(10 templates SOURCES templates.cpp)

# ---- 第 12 课：调试。结果故意是错的，但程序正常结束 ----
cpptut_example(12 debug_me SOURCES debug_me.cpp)

# ---- 第 13 课：内存分析 ----
cpptut_example(13 overflow SOURCES overflow.cpp BUILD_ONLY)
cpptut_example(13 leak SOURCES leak.cpp BUILD_ONLY)
cpptut_example(13 int_overflow SOURCES int_overflow.cpp SANITIZE undefined BUILD_ONLY)
cpptut_example(13 leak_fixed SOURCES leak_fixed.cpp)
cpptut_example(13 growth SOURCES growth.cpp)

# ---- 第 14 课：性能分析；第 14 课延伸：内存池 ----
cpptut_example(14 benchmark SOURCES benchmark.cpp OPTIMIZE)
cpptut_example(14a alloc_cost SOURCES alloc_cost.cpp OPTIMIZE)
cpptut_example(14a block_pool SOURCES block_pool.cpp)
cpptut_example(14a buffer_pool SOURCES buffer_pool.cpp SANITIZE thread)
if(CPPTUT_SANITIZERS)
  cpptut_example(14a buffer_pool SOURCES buffer_pool.cpp SANITIZE address SUFFIX _asan)
endif()

# ---- 第 15 课预备：线程和进程；第 15 课：并发基础 ----
cpptut_example(15a address_space SOURCES address_space.cpp SANITIZE thread)
cpptut_example(15a context_switch SOURCES context_switch.cpp SANITIZE thread)
cpptut_example(15a why_threads SOURCES why_threads.cpp OPTIMIZE)
cpptut_example(15 race SOURCES race.cpp SANITIZE thread BUILD_ONLY)
cpptut_example(15 sync SOURCES sync.cpp SANITIZE thread)

# ---- 第 16 课：promise、future 与推理服务；第 16 课延伸：线程池 ----
cpptut_example(16 future_basics SOURCES future_basics.cpp SANITIZE thread)
cpptut_example(16 inference_worker SOURCES inference_worker.cpp SANITIZE thread)
if(CPPTUT_SANITIZERS)
  cpptut_example(16 inference_worker SOURCES inference_worker.cpp SANITIZE address SUFFIX _asan)
endif()
cpptut_example(16a pitfalls SOURCES pitfalls.cpp SANITIZE thread)
cpptut_example(16a parallel_nodes SOURCES parallel_nodes.cpp SANITIZE thread)
cpptut_example(16a pool_vs_spawn SOURCES pool_vs_spawn.cpp OPTIMIZE)

# ---- 第 17 课：日志与崩溃转储。故意崩溃的程序，只编译 ----
cpptut_example(17 crash SOURCES crash.cpp SANITIZE none BUILD_ONLY)
