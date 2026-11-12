# 被驱动适配器：技术细节都在这里，实现领域层或应用层定义的端口。

# 图像：共享内核 Image ↔ OpenCV
add_library(vision_imaging STATIC imaging/src/cv_bridge.cpp)
target_include_directories(vision_imaging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/imaging/include)
target_link_libraries(vision_imaging PUBLIC vision_kernel ${OpenCV_LIBS})
target_include_directories(vision_imaging SYSTEM PUBLIC ${OpenCV_INCLUDE_DIRS})

# 进程内事件总线
add_library(vision_eventbus STATIC eventbus/src/in_memory_event_bus.cpp)
target_include_directories(vision_eventbus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/eventbus/include)
target_link_libraries(vision_eventbus PUBLIC vision_common)

# JSON 文件仓储：实现方案编排领域层的仓储接口，所以可以依赖 authoring_domain
add_library(vision_persistence STATIC persistence/src/json_repositories.cpp)
target_include_directories(vision_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/persistence/include)
target_link_libraries(vision_persistence
  PUBLIC authoring_domain authoring_application
  PRIVATE nlohmann_json::nlohmann_json)
