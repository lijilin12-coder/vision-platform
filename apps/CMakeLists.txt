# 驱动适配器 + 组装根
add_executable(vision-cli cli/main.cpp cli/station_commands.cpp cli/assist_commands.cpp cli/data_commands.cpp)
target_link_libraries(vision-cli PRIVATE
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
  catalog_application
  vision_operators
  vision_imaging
  vision_eventbus
  vision_persistence
  vision_logging
  vision_crash
  nlohmann_json::nlohmann_json)

if(WIN32)
  target_link_libraries(vision-cli PRIVATE shell32)  # CommandLineToArgvW
endif()

# 导出符号：这样崩溃报告里的调用栈能显示函数名，而不只是地址（macOS / Linux）。
if(NOT WIN32)
  set_target_properties(vision-cli PROPERTIES ENABLE_EXPORTS ON)
endif()
