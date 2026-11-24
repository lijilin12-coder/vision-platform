# 驱动适配器 + 组装根
# 两个程序共用的组装（apps/common/platform.h）：整个平台需要的全部库。
add_library(vision_platform_assembly INTERFACE)
target_include_directories(vision_platform_assembly INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(vision_platform_assembly INTERFACE
  authoring_application
  execution_application
  release_application
  station_application
  assistant_application
  vision_assist_adapters
  vision_content
  data_application
  models_application
  training_application
  vision_llm
  vision_training_http
  catalog_application
  vision_imaging
  vision_eventbus
  vision_persistence
  vision_logging
  vision_crash
  nlohmann_json::nlohmann_json)
if(VISION_WITH_ONNXRUNTIME)
  target_link_libraries(vision_platform_assembly INTERFACE vision_inference_onnx)
  target_compile_definitions(vision_platform_assembly INTERFACE VISION_WITH_ONNXRUNTIME)
endif()
if(WIN32)
  target_link_libraries(vision_platform_assembly INTERFACE shell32)  # CommandLineToArgvW
endif()

# ---- 命令行 ----
add_executable(vision-cli cli/main.cpp cli/station_commands.cpp cli/assist_commands.cpp cli/data_commands.cpp)
target_link_libraries(vision-cli PRIVATE vision_platform_assembly vision_operators)
vision_copy_onnxruntime(vision-cli)

# ---- 服务：HTTP 接口（一个库，测试也要链接它）+ 入口程序 ----
add_library(vision_http_api STATIC runtime/http_api.cpp)
target_include_directories(vision_http_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
target_link_libraries(vision_http_api PUBLIC vision_platform_assembly PRIVATE httplib::httplib)

add_executable(vision-runtime runtime/main.cpp)
# vision_operators 是 OBJECT 库（算子靠全局对象的构造函数注册自己）：它的目标文件只会进入【直接】链接它的目标，
# 不会隔着 vision_http_api 这个静态库传过来。所以每个可执行程序都要自己直接链接一次，否则算子目录是空的。
target_link_libraries(vision-runtime PRIVATE vision_http_api vision_operators)
vision_copy_onnxruntime(vision-runtime)

# 导出符号：这样崩溃报告里的调用栈能显示函数名，而不只是地址（macOS / Linux）。
if(NOT WIN32)
  set_target_properties(vision-cli PROPERTIES ENABLE_EXPORTS ON)
endif()
