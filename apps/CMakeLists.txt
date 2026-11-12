# 驱动适配器 + 组装根
add_executable(vision-cli cli/main.cpp)
target_link_libraries(vision-cli PRIVATE
  authoring_application
  execution_application
  catalog_application
  vision_operators
  vision_imaging
  vision_eventbus
  vision_persistence
  nlohmann_json::nlohmann_json)
