# 技术通用组件：Result、ID、领域事件基类、日志接口。不包含任何业务概念。
add_library(vision_common STATIC src/id.cpp src/log.cpp)
target_include_directories(vision_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
