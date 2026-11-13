# OpenCV 教程的示例。
#
# 两种用法：
#   1. 作为整个项目的一部分（推荐）：在仓库根目录按 docs/dev/build.md 构建，示例会一起编译，
#      并且作为测试运行（ctest）。
#   2. 单独构建：cmake -S docs/tutorial/opencv/examples -B build-cvtut && cmake --build build-cvtut
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(opencv_tutorial LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(MSVC)
    add_compile_options(/W4 /utf-8)
  else()
    add_compile_options(-Wall -Wextra -Wpedantic)
  endif()
  find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
  enable_testing()
endif()

# 示例把结果图像写到这个目录，运行之后打开来看
set(CVTUT_OUT_DIR "${CMAKE_BINARY_DIR}/tutorial-out/opencv")

# cvtut_example(<课号> <名字>)：编译 lessonNN/<名字>.cpp，并注册成一个测试
function(cvtut_example lesson name)
  set(target "cv${lesson}_${name}")
  add_executable(${target} "lesson${lesson}/${name}.cpp")
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
  target_link_libraries(${target} PRIVATE ${OpenCV_LIBS})
  add_test(NAME ${target} COMMAND ${target} "${CVTUT_OUT_DIR}/lesson${lesson}")
endfunction()

cvtut_example(01 mat_basics)
cvtut_example(01 read_write)
cvtut_example(02 channels)
cvtut_example(02 depth)
cvtut_example(03 histogram)
cvtut_example(03 threshold)
cvtut_example(04 filter)
cvtut_example(04 morphology)
