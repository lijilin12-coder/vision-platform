# BC-11 方案助手（核心域）。按工作流搭出候选方案，驱动“提议 → 应用 → 实验 → 评估”的循环。
#
# 它只在方案构建平台上使用：产线运行的程序不链接它（ADR-28）。
# 应用层不依赖任何其他上下文：需要的能力（编辑方案、试运行、基准图集、评估、顾问……）
# 都通过它自己定义的端口取得，由组装根接上具体的实现。
vision_domain_library(assistant SOURCES
  domain/src/workflow.cpp
  domain/src/session.cpp)

vision_application_library(assistant
  SOURCES application/src/assist_service.cpp application/src/report_markdown.cpp)
