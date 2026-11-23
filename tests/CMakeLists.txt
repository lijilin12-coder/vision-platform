include(GoogleTest)

# ---- 领域层单元测试：只链接领域库，不链接 OpenCV ----
add_executable(kernel_tests kernel_test.cpp)
target_link_libraries(kernel_tests PRIVATE vision_kernel GTest::gtest_main)
gtest_discover_tests(kernel_tests)

add_executable(authoring_domain_tests authoring_domain_test.cpp)
target_include_directories(authoring_domain_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(authoring_domain_tests PRIVATE authoring_domain GTest::gtest_main)
gtest_discover_tests(authoring_domain_tests)

add_executable(execution_domain_tests execution_domain_test.cpp)
target_include_directories(execution_domain_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(execution_domain_tests PRIVATE execution_domain GTest::gtest_main)
gtest_discover_tests(execution_domain_tests)

add_executable(release_domain_tests release_domain_test.cpp)
target_link_libraries(release_domain_tests PRIVATE release_domain GTest::gtest_main)
gtest_discover_tests(release_domain_tests)

add_executable(station_domain_tests station_domain_test.cpp)
target_link_libraries(station_domain_tests PRIVATE station_domain GTest::gtest_main)
gtest_discover_tests(station_domain_tests)

add_executable(catalog_tests catalog_test.cpp)
target_link_libraries(catalog_tests PRIVATE catalog_application GTest::gtest_main)
gtest_discover_tests(catalog_tests)

# ---- 集成测试：真实算子 + OpenCV + JSON 仓储 ----
add_executable(integration_tests integration_test.cpp)
target_link_libraries(integration_tests PRIVATE
  authoring_application execution_application catalog_application
  vision_operators vision_imaging vision_eventbus vision_persistence
  GTest::gtest_main)
gtest_discover_tests(integration_tests)

# ---- 工位运行（POC-9）：热切换、回滚、热启动，含 10,000 次检测的并发测试 ----
add_executable(station_integration_tests station_integration_test.cpp)
target_link_libraries(station_integration_tests PRIVATE
  station_application release_application authoring_application execution_application catalog_application
  vision_operators vision_imaging vision_eventbus vision_persistence
  GTest::gtest_main)
gtest_discover_tests(station_integration_tests)

# ---- 日志 ----
add_executable(logging_tests logging_test.cpp)
target_link_libraries(logging_tests PRIVATE vision_logging GTest::gtest_main)
gtest_discover_tests(logging_tests)

# ---- 崩溃处理：故意让程序崩溃，检查留下的崩溃报告、转储文件和日志 ----
foreach(kind segv abort throw)
  add_test(NAME crash_leaves_report_${kind}
           COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:vision-cli> -DWORK_DIR=${CMAKE_BINARY_DIR}/crash-test-${kind}
                   -DKIND=${kind} -P ${CMAKE_CURRENT_SOURCE_DIR}/crash/check_crash.cmake)
endforeach()

# ---- 边界测试：故意违反边界的代码必须编译失败 ----
add_library(boundary_violation OBJECT EXCLUDE_FROM_ALL boundary/violation.cpp)
target_link_libraries(boundary_violation PRIVATE authoring_application)
add_test(NAME boundary_violation_fails_to_compile
         COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=boundary_violation
                 -DEXPECT=vision/execution/domain/inspection.h
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/boundary/check_violation.cmake)

# 产线运行平台用到的库（工位运行、上线管理、检测执行）够不着方案助手（ADR-28，POC-11）
add_library(boundary_runtime_uses_assistant OBJECT EXCLUDE_FROM_ALL boundary/runtime_uses_assistant.cpp)
target_link_libraries(boundary_runtime_uses_assistant PRIVATE station_application release_application execution_application)
add_test(NAME boundary_runtime_cannot_use_assistant
         COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=boundary_runtime_uses_assistant
                 -DEXPECT=vision/assistant/api.h
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/boundary/check_violation.cmake)
# 两个边界测试都会调用构建工具，不能同时进行
set_tests_properties(boundary_violation_fails_to_compile boundary_runtime_cannot_use_assistant
                     PROPERTIES RESOURCE_LOCK boundary_build)

# ---- 方案助手（POC-11）的领域层：只链接领域库，没有 OpenCV，也没有网络 ----
add_executable(assistant_domain_tests assistant_domain_test.cpp)
target_link_libraries(assistant_domain_tests PRIVATE assistant_domain GTest::gtest_main)
gtest_discover_tests(assistant_domain_tests)

# ---- 方案助手的集成测试：真实算子 + 检测引擎 + JSON 文件存储 + 按脚本出牌的顾问 ----
add_executable(assistant_integration_tests assistant_integration_test.cpp)
target_link_libraries(assistant_integration_tests PRIVATE
  vision_assist_adapters vision_llm assistant_application authoring_application execution_application release_application
  catalog_application vision_operators vision_imaging vision_eventbus vision_persistence
  nlohmann_json::nlohmann_json GTest::gtest_main)
target_compile_definitions(assistant_integration_tests PRIVATE VISION_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
gtest_discover_tests(assistant_integration_tests)

# ---- 大模型顾问（POC-12）：用假的 HTTP 客户端，不访问网络 ----
add_executable(llm_advisor_tests llm_advisor_test.cpp)
target_link_libraries(llm_advisor_tests PRIVATE vision_llm nlohmann_json::nlohmann_json GTest::gtest_main)
gtest_discover_tests(llm_advisor_tests)

# ---- 算子：直接创建算子、喂给它合成的图像、检查输出 ----
add_executable(operators_tests operators_test.cpp)
target_link_libraries(operators_tests PRIVATE catalog_application vision_operators vision_imaging GTest::gtest_main)
gtest_discover_tests(operators_tests)

# ---- 匹配模板机制：内容寻址存储、模板随方案版本冻结、从方案一路传到算子、随上线包部署 ----
add_executable(template_integration_tests template_integration_test.cpp)
target_link_libraries(template_integration_tests PRIVATE
  station_application release_application authoring_application execution_application catalog_application
  vision_operators vision_imaging vision_eventbus vision_persistence vision_content
  GTest::gtest_main)
gtest_discover_tests(template_integration_tests)
