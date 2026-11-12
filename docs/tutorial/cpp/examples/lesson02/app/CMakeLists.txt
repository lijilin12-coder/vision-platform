add_executable(overlap_app main.cpp)

# 链接 geometry。因为 geometry 的 include 目录是 PUBLIC 的，这里不需要再写头文件路径。
target_link_libraries(overlap_app PRIVATE geometry)
