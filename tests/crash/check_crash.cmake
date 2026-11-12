# 故意让 vision-cli 崩溃，检查崩溃之后留下了什么：
#   1. 程序以非零退出码结束
#   2. 崩溃目录里有崩溃报告；Windows 上还要有非空的 minidump
#   3. 日志文件里有崩溃前写下的最后一条日志（说明日志没有留在缓冲区里丢掉）
#
# 用法：cmake -DCLI=<vision-cli 路径> -DWORK_DIR=<临时目录> -DKIND=<segv|abort|throw> -P check_crash.cmake

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(
  COMMAND "${CLI}" --data "${WORK_DIR}" --no-core-dump debug crash ${KIND}
  WORKING_DIRECTORY "${WORK_DIR}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(result STREQUAL "0")
  message(FATAL_ERROR "程序本应崩溃，却正常退出了")
endif()

file(GLOB reports "${WORK_DIR}/crashes/crash-vision-cli-*.txt")
list(LENGTH reports reportCount)
if(NOT reportCount EQUAL 1)
  message(FATAL_ERROR "应该恰好有 1 份崩溃报告，实际有 ${reportCount} 份。\n退出码：${result}\n${out}\n${err}")
endif()
list(GET reports 0 report)
file(READ "${report}" reportText)
if(NOT reportText MATCHES "vision crash report")
  message(FATAL_ERROR "崩溃报告的内容不对：\n${reportText}")
endif()

if(WIN32)
  file(GLOB dumps "${WORK_DIR}/crashes/crash-vision-cli-*.dmp")
  list(LENGTH dumps dumpCount)
  if(NOT dumpCount EQUAL 1)
    message(FATAL_ERROR "应该恰好有 1 个 minidump，实际有 ${dumpCount} 个")
  endif()
  list(GET dumps 0 dump)
  file(SIZE "${dump}" dumpSize)
  if(dumpSize LESS 1024)
    message(FATAL_ERROR "minidump 文件太小（${dumpSize} 字节），可能没有写成功")
  endif()
  message(STATUS "minidump：${dumpSize} 字节")
else()
  if(KIND STREQUAL "segv" AND NOT reportText MATCHES "SIGSEGV")
    message(FATAL_ERROR "崩溃报告里应该有 SIGSEGV：\n${reportText}")
  endif()
  if(NOT KIND STREQUAL "segv" AND NOT reportText MATCHES "SIGABRT")
    message(FATAL_ERROR "崩溃报告里应该有 SIGABRT：\n${reportText}")
  endif()
endif()

file(READ "${WORK_DIR}/logs/vision-cli.log" logText)
if(NOT logText MATCHES "kind=${KIND}")
  message(FATAL_ERROR "日志里没有崩溃前的最后一条记录：\n${logText}")
endif()
if(KIND STREQUAL "throw" AND NOT logText MATCHES "ERROR \\[crash\\]")
  message(FATAL_ERROR "未捕获的异常应该被记入日志：\n${logText}")
endif()

message(STATUS "崩溃类型 ${KIND}：崩溃报告和日志都按预期留下了")
