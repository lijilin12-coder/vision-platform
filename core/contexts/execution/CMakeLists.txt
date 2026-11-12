# BC-3 检测执行（核心域）
vision_domain_library(execution SOURCES
  domain/src/plan.cpp
  domain/src/inspection.cpp
  domain/src/executor.cpp)

# 依赖 authoring（只用发布语言）和 catalog（算子实例化）的公开接口
vision_application_library(execution
  SOURCES application/src/execution_service.cpp
  DEPENDS authoring catalog)
