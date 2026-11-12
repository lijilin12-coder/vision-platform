# 算子：通用域的封装，使用策略模式（不使用 DDD 战术模式）。
#
# 新增算子只需在 src/ 下新建一个 .cpp 文件（NFR-07），不用修改本文件。
#
# 使用 OBJECT 库：静态注册依赖全局对象的构造函数。如果打包成普通静态库，
# 链接器会丢弃“没有被引用”的算子目标文件，导致算子没有注册。
file(GLOB VISION_OPERATOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(vision_operators OBJECT ${VISION_OPERATOR_SOURCES})
target_link_libraries(vision_operators PUBLIC catalog_application vision_imaging)
