cmake_minimum_required(VERSION 3.20)
project(vision_platform VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VISION_BUILD_TESTS "Build unit and integration tests" ON)
option(VISION_BUILD_TUTORIALS "Build the OpenCV tutorial examples" ON)
option(VISION_WITH_ONNXRUNTIME "Run models with ONNX Runtime (downloads the prebuilt library)" ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(cmake/VisionBoundaries.cmake)

# 只有算子和适配器需要 OpenCV；领域层不允许依赖它（见 docs/02-architecture.md §4.3）
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(nlohmann_json 3 REQUIRED)

if(VISION_WITH_ONNXRUNTIME)
  include(cmake/OnnxRuntime.cmake)
else()
  # 关掉推理功能时，这个函数什么也不做，调用它的地方不用再判断
  function(vision_copy_onnxruntime target)
  endfunction()
endif()

if(VISION_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
endif()

add_subdirectory(core)
add_subdirectory(operators)
add_subdirectory(adapters)
add_subdirectory(apps)

if(VISION_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(VISION_BUILD_TUTORIALS)
  enable_testing()
  add_subdirectory(docs/tutorial/opencv/examples)
endif()

# 配置阶段检查：所有领域层目标只能依赖共享内核和 common
vision_check_domain_purity()
