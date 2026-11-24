# 驱动适配器 + 组装根
# 组装分两层（apps/common/）：
#   vision_line_assembly      产线运行平台（line_platform.h）：检测执行、上线管理、工位、模型仓库、方案包
#   vision_platform_assembly  方案构建平台（platform.h）：在上面那一层之外，再加方案编排、助手、数据、训练
add_library(vision_line_assembly INTERFACE)
target_include_directories(vision_line_assembly INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(vision_line_assembly INTERFACE
  execution_application
  release_application
  station_application
  models_application
  catalog_application
  vision_content
  vision_exchange
  vision_image_sources
  vision_imaging
  vision_eventbus
  vision_persistence
  vision_logging
  vision_crash
  nlohmann_json::nlohmann_json)
if(VISION_WITH_ONNXRUNTIME)
  target_link_libraries(vision_line_assembly INTERFACE vision_inference_onnx)
  target_compile_definitions(vision_line_assembly INTERFACE VISION_WITH_ONNXRUNTIME)
endif()
if(WIN32)
  target_link_libraries(vision_line_assembly INTERFACE shell32)  # CommandLineToArgvW
endif()

add_library(vision_platform_assembly INTERFACE)
target_link_libraries(vision_platform_assembly INTERFACE
  vision_line_assembly
  authoring_application
  assistant_application
  vision_assist_adapters
  data_application
  training_application
  vision_llm
  vision_training_http)

# 两个命令行程序共用的命令：上线包、工位、方案包、让工位持续运行
# 让工位持续运行的部件：命令行（vision-line serve）和服务（给界面用）共用
add_library(vision_station_runner STATIC common/station_runner.cpp)
target_link_libraries(vision_station_runner PUBLIC vision_line_assembly)

add_library(vision_line_commands STATIC common/line_commands.cpp common/cli_util.cpp)
target_link_libraries(vision_line_commands PUBLIC vision_line_assembly vision_station_runner)

# ---- 命令行 ----
add_executable(vision-cli cli/main.cpp cli/assist_commands.cpp cli/data_commands.cpp)
target_link_libraries(vision-cli PRIVATE vision_platform_assembly vision_line_commands vision_operators)
vision_copy_onnxruntime(vision-cli)

# ---- 产线运行平台 ----
# 注意它链接的是 vision_line_assembly，不是 vision_platform_assembly：组装里没有方案编排、助手、数据、训练
add_library(vision_line_api STATIC line/line_api.cpp)
target_include_directories(vision_line_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/line)
target_link_libraries(vision_line_api PUBLIC vision_line_assembly PRIVATE httplib::httplib vision_http)

add_executable(vision-line line/main.cpp)
target_link_libraries(vision-line PRIVATE vision_line_commands vision_line_api vision_operators)
vision_copy_onnxruntime(vision-line)

# ---- 服务：HTTP 接口（一个库，测试也要链接它）+ 入口程序 ----
add_library(vision_http_api STATIC runtime/http_api.cpp)
target_include_directories(vision_http_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
target_link_libraries(vision_http_api PUBLIC vision_platform_assembly vision_station_runner PRIVATE httplib::httplib)

add_executable(vision-runtime runtime/main.cpp)
# vision_operators 是 OBJECT 库（算子靠全局对象的构造函数注册自己）：它的目标文件只会进入【直接】链接它的目标，
# 不会隔着 vision_http_api 这个静态库传过来。所以每个可执行程序都要自己直接链接一次，否则算子目录是空的。
target_link_libraries(vision-runtime PRIVATE vision_http_api vision_operators)
vision_copy_onnxruntime(vision-runtime)

# 导出符号：这样崩溃报告里的调用栈能显示函数名，而不只是地址（macOS / Linux）。
if(NOT WIN32)
  set_target_properties(vision-cli vision-line PROPERTIES ENABLE_EXPORTS ON)
endif()
