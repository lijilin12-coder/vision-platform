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
  PUBLIC authoring_domain authoring_application release_domain release_application station_domain data_domain models_domain training_domain quality_domain
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

# HTTP 客户端：通过系统自带的 curl 命令发请求，不增加第三方依赖。大模型顾问和训练服务都用它。
add_library(vision_http STATIC http/src/http_client.cpp)
target_include_directories(vision_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/http/include)
target_link_libraries(vision_http PUBLIC vision_common)

# 训练后端的一个实现：通过 HTTP 调我们自己的训练服务（services/training）。只有模型训练平台使用。
add_library(vision_training_http STATIC training_http/src/http_training_backend.cpp)
target_include_directories(vision_training_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/training_http/include)
target_link_libraries(vision_training_http
  PUBLIC training_domain vision_http
  PRIVATE nlohmann_json::nlohmann_json)

# 大模型顾问（只有方案构建平台使用）：方案顾问接口的又一个实现。
#   - 两种对话协议：Anthropic（Claude）、OpenAI 兼容（DeepSeek、通义千问等）
add_library(vision_llm STATIC
  llm/src/chat_protocol.cpp
  llm/src/llm_advisor.cpp
  llm/src/image_loader.cpp)
target_include_directories(vision_llm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/llm/include)
target_link_libraries(vision_llm
  PUBLIC assistant_domain vision_http
  PRIVATE nlohmann_json::nlohmann_json ${OpenCV_LIBS})
target_include_directories(vision_llm SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})

# 内容寻址存储（ADR-20）和匹配模板的存取。
# 方案构建平台用它保存模板；两个平台都用它在加载方案时读取模板。
add_library(vision_content STATIC content/src/content_store.cpp)
target_include_directories(vision_content PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/content/include)
target_link_libraries(vision_content
  PUBLIC authoring_application execution_application data_application release_application
         models_application training_application training_domain quality_application
  PRIVATE vision_imaging)

# 推理引擎：用 ONNX Runtime 运行模型仓库里的模型，实现检测执行的 ModelSource 端口。
# 推理引擎只出现在这一个库里；算子通过共享内核的 InferenceModel 接口使用模型，不知道背后是谁。
if(VISION_WITH_ONNXRUNTIME)
  add_library(vision_inference_onnx STATIC inference_onnx/src/onnx_model_source.cpp)
  target_include_directories(vision_inference_onnx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inference_onnx/include)
  target_link_libraries(vision_inference_onnx
    PUBLIC execution_application models_application
    PRIVATE vision_content onnxruntime::onnxruntime)
endif()

# 图像源：工位的图像从哪里来。现在只有“一个目录里的图像”；相机以后加在这里。
add_library(vision_image_sources STATIC image_sources/src/folder_source.cpp)
target_include_directories(vision_image_sources PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/image_sources/include)
target_link_libraries(vision_image_sources PUBLIC station_application PRIVATE vision_imaging)

# 方案包文件（REL-02）：把方案版本连同模板、模型打成一个 tar 文件，导出和导入。
# 传输方式是适配器的事；导入的规则在上线管理的领域层。
add_library(vision_exchange STATIC exchange/src/package_file.cpp)
target_include_directories(vision_exchange PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/exchange/include)
target_link_libraries(vision_exchange
  PUBLIC release_application models_application catalog_application vision_content
  PRIVATE vision_persistence nlohmann_json::nlohmann_json)
