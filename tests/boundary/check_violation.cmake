# 构建故意违规的目标，要求：
#   1. 构建失败
#   2. 失败原因是找不到另一个上下文的领域头文件（而不是别的编译错误）
execute_process(
  COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --target boundary_violation
  RESULT_VARIABLE result
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(result EQUAL 0)
  message(FATAL_ERROR "边界没有被强制：违规代码竟然编译通过了")
endif()

string(FIND "${out}${err}" "vision/execution/domain/inspection.h" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "构建失败了，但原因不是跨上下文引用领域头文件：\n${out}${err}")
endif()

message(STATUS "边界有效：方案编排无法引用检测执行的领域类")
