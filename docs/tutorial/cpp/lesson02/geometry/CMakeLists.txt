# 定义一个静态库，名字叫 geometry，由下面两个源文件编译而成
add_library(geometry STATIC
  src/rect.cpp
  src/overlap.cpp)

# PUBLIC：include/ 目录既给库自己用，也“传递”给所有链接了 geometry 的目标。
#         所以使用者可以直接写 #include "geometry/rect.h"。
# PRIVATE：src/ 目录只给库自己用。使用者看不到里面的 detail.h。
target_include_directories(geometry
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
