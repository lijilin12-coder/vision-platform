# 第 2 课示例：一个最小但结构完整的 CMake 项目。
#
#   geometry/   一个库：对外的头文件在 include/，实现在 src/
#   app/        一个可执行程序，使用 geometry 库
#   tests/      一个测试程序，也使用 geometry 库
cmake_minimum_required(VERSION 3.20)
project(lesson02 LANGUAGES CXX)

# 整个项目统一使用 C++17，并且不使用编译器的私有扩展
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 打开警告。MSVC 和其他编译器的选项写法不同。
if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

enable_testing()

add_subdirectory(geometry)
add_subdirectory(app)
add_subdirectory(tests)
