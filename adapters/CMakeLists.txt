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
add_library(vision_persistence STATIC
  persistence/src/json_repositories.cpp
  persistence/src/runtime_repositories.cpp)
target_include_directories(vision_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/persistence/include)
target_link_libraries(vision_persistence
  PUBLIC authoring_domain authoring_application release_domain station_domain
  PRIVATE nlohmann_json::nlohmann_json)

# 日志接收器：控制台、轮转文件
add_library(vision_logging STATIC logging/src/sinks.cpp)
target_include_directories(vision_logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/logging/include)
target_link_libraries(vision_logging PUBLIC vision_common)

# 崩溃处理：Windows 写 minidump，macOS / Linux 写崩溃报告并放开 core 文件限制
add_library(vision_crash STATIC crash/src/crash_handler.cpp)
target_include_directories(vision_crash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/crash/include)
target_link_libraries(vision_crash PUBLIC vision_common)
if(WIN32)
  target_link_libraries(vision_crash PRIVATE dbghelp)
endif()

# 方案助手的适配器（只有方案构建平台使用）：
#   - 把助手的端口接到方案编排、检测执行、上线管理上
#   - 两个不需要大模型的顾问：按脚本出牌的、网格搜索的
#   - 工作流和助手会话的 JSON 文件存储
add_library(vision_assist_adapters STATIC
  assistant/src/platform_ports.cpp
  assistant/src/scripted_advisor.cpp
  assistant/src/search_advisor.cpp
  assistant/src/storage.cpp)
target_include_directories(vision_assist_adapters
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/assistant/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/persistence/src)
target_link_libraries(vision_assist_adapters
  PUBLIC assistant_domain assistant_application authoring_application execution_application release_application
  PRIVATE vision_imaging nlohmann_json::nlohmann_json)
