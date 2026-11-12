# 限界上下文的边界规则（ADR-17，docs/02-architecture.md §4.3）
#
# 每个上下文拆成两个目标：
#   <ctx>_domain       领域层：只能依赖 vision_kernel、vision_common
#   <ctx>_application  应用层：私有地依赖本上下文的 domain；可以依赖其他上下文的 application
#
# 关键手段：application 以 PRIVATE 方式链接 domain，所以 domain 的头文件路径
# 不会传递给使用者。任何上下文（或应用）如果试图 #include 另一个上下文的领域头文件，
# 会因为找不到头文件而编译失败。

set(VISION_DOMAIN_ALLOWED_DEPS vision_kernel vision_common)

# vision_domain_library(<ctx> SOURCES ...)
function(vision_domain_library ctx)
  cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
  set(target ${ctx}_domain)
  add_library(${target} STATIC ${ARG_SOURCES})
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/domain/include)
  target_link_libraries(${target} PUBLIC vision_kernel vision_common)
  set_property(GLOBAL APPEND PROPERTY VISION_DOMAIN_TARGETS ${target})
endfunction()

# vision_application_library(<ctx> SOURCES ... [DEPENDS <other ctx> ...])
function(vision_application_library ctx)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
  set(target ${ctx}_application)
  add_library(${target} STATIC ${ARG_SOURCES})
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/application/include)
  target_link_libraries(${target}
    PUBLIC vision_kernel vision_common
    PRIVATE ${ctx}_domain)
  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries(${target} PUBLIC ${dep}_application)
  endforeach()
endfunction()

# 检查所有领域层目标的直接依赖
function(vision_check_domain_purity)
  get_property(domains GLOBAL PROPERTY VISION_DOMAIN_TARGETS)
  foreach(target IN LISTS domains)
    get_target_property(deps ${target} LINK_LIBRARIES)
    foreach(dep IN LISTS deps)
      if(NOT dep IN_LIST VISION_DOMAIN_ALLOWED_DEPS)
        message(FATAL_ERROR
          "边界违规：领域层目标 ${target} 依赖了 ${dep}。"
          "领域层只能依赖：${VISION_DOMAIN_ALLOWED_DEPS}")
      endif()
    endforeach()
  endforeach()
endfunction()
