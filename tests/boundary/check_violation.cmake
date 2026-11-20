# 构建故意违规的目标，要求：
#   1. 构建失败
#   2. 失败原因是找不到 EXPECT 指定的头文件（而不是别的编译错误）
# 参数：BUILD_DIR、TARGET、EXPECT
execute_process(
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target ${TARGET}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(result EQUAL 0)
  message(FATAL_ERROR "边界没有被强制：违规代码竟然编译通过了")
endif()

string(FIND "${out}${err}" "${EXPECT}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "构建失败了，但原因不是找不到 ${EXPECT}：\n${out}${err}")
endif()

message(STATUS "边界有效：${TARGET} 无法包含 ${EXPECT}")
