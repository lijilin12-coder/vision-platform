# 共享内核：视觉基础值对象与算子签名。BC-1、BC-2、BC-3 共用，必须保持很小。
# 不依赖 OpenCV。
add_library(vision_kernel STATIC src/kernel.cpp)
target_include_directories(vision_kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vision_kernel PUBLIC vision_common)
