# 推理引擎与大模型推理优化教程的全部示例。只用到标准库，不需要安装任何依赖。
#
#   配置：        cmake --preset default
#   编译全部：    cmake --build --preset default
#   运行全部：    ctest --preset default
#   只编译一个：  cmake --build --preset default --target l05_matmul
#   运行一个：    build/bin/l05_matmul        （Windows：build\bin\l05_matmul.exe）
#
# 目标的名字是 “l课号_文件名”。每一课是一个文件夹，里面有自己的 CMakeLists.txt，
# 用下面定义的 inftut_example() 声明这一课的示例。
cmake_minimum_required(VERSION 3.21)
project(inference_tutorial LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)   # 默认带调试信息、不优化：方便用调试器。测量时间的示例单独打开优化
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")   # 所有可执行程序放在同一个目录，好找

enable_testing()
find_package(Threads REQUIRED)

if(MSVC)
  # 测量时间的示例要打开优化（/O2）。MSVC 的调试配置默认带 /RTC1，它和 /O2 不能同时使用，所以去掉。
  string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  set(INFTUT_WARNINGS /W4 /utf-8 /EHsc)
else()
  set(INFTUT_WARNINGS -Wall -Wextra -Wpedantic)
endif()

# 把警告当成错误。自动构建时打开（run_all.sh 会打开它）；平时默认关闭。
option(INFTUT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(INFTUT_WARNINGS_AS_ERRORS AND NOT MSVC)
  list(APPEND INFTUT_WARNINGS -Werror)
endif()

# 内存检查、线程检查工具（AddressSanitizer、ThreadSanitizer）：macOS、Linux 上默认启用，Windows 上不加。
# 它们是什么，见 C++ 教程第 13、15 课。
if(UNIX)
  set(INFTUT_SANITIZERS ON)
else()
  set(INFTUT_SANITIZERS OFF)
endif()

# inftut_example(<课号> <名字> SOURCES <源文件...>
#                [SANITIZE address|thread|undefined|none]   用哪种检查工具编译（macOS、Linux 上有效；默认 address）
#                [OPTIMIZE]                                 打开优化，并且不加检查工具（测量时间的示例需要）
#                [NATIVE]                                   允许编译器使用本机 CPU 的全部指令（-march=native；MSVC 上是 /arch:AVX2）
#                [BUILD_ONLY]                               只编译，不作为测试运行
#                [ARGS <参数...>])                          作为测试运行时传给程序的命令行参数
# 源文件的路径相对于调用它的那个 CMakeLists.txt。公共头文件在 common/ 下：#include "inftut/common.h"
function(inftut_example lesson name)
  cmake_parse_arguments(A "OPTIMIZE;NATIVE;BUILD_ONLY" "SANITIZE" "SOURCES;ARGS" ${ARGN})
  if(NOT A_SANITIZE)
    set(A_SANITIZE address)
  endif()
  set(target "l${lesson}_${name}")
  add_executable(${target} ${A_SOURCES})
  target_compile_options(${target} PRIVATE ${INFTUT_WARNINGS})
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/common")
  target_link_libraries(${target} PRIVATE Threads::Threads)

  if(A_OPTIMIZE)
    target_compile_options(${target} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/O2,-O2>)
  endif()
  if(A_NATIVE)
    if(MSVC)
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        target_compile_options(${target} PRIVATE /arch:AVX2)
      endif()
    else()
      target_compile_options(${target} PRIVATE -march=native)
    endif()
  endif()

  if(INFTUT_SANITIZERS AND NOT A_OPTIMIZE AND NOT A_SANITIZE STREQUAL "none")
    if(A_SANITIZE STREQUAL "address")
      set(flags -fsanitize=address,undefined)
    else()
      set(flags -fsanitize=${A_SANITIZE})
    endif()
    target_compile_options(${target} PRIVATE ${flags})
    target_link_options(${target} PRIVATE ${flags})
  endif()

  if(NOT A_BUILD_ONLY)
    add_test(NAME ${target} COMMAND ${target} ${A_ARGS})
    if(A_SANITIZE STREQUAL "thread")
      # 屏蔽标准库里一处已知的误报，原因见 C++ 教程的 tsan_suppressions.txt
      set_tests_properties(${target} PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/../cpp/tsan_suppressions.txt")
    endif()
  endif()
endfunction()

# 每一课一个文件夹。第 00 课只有正文，没有示例。
foreach(lesson 01 02 03 04 05 06 07 08 09 10 11 12 13 14)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lesson${lesson}/CMakeLists.txt")
    add_subdirectory(lesson${lesson})
  endif()
endforeach()
