# 运行 PROGRAM，把文件 INPUT 作为它的标准输入，检查输出（去掉首尾空白）是否等于 EXPECTED。
#   cmake -DPROGRAM=... -DINPUT=... -DEXPECTED=... -P check_stdin.cmake
execute_process(COMMAND "${PROGRAM}" INPUT_FILE "${INPUT}" OUTPUT_VARIABLE output RESULT_VARIABLE code)
string(STRIP "${output}" output)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "程序的退出码是 ${code}")
endif()
if(NOT output STREQUAL "${EXPECTED}")
  message(FATAL_ERROR "期望输出 [${EXPECTED}]，实际输出 [${output}]")
endif()
