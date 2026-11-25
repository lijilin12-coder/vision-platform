# BC-10 质量分析（支撑域）。目前是最小的一部分：
#   检测记录（每次产线检测留下一条，可追溯到上线包和方案版本）、查询、汇总（总数、良率、缺陷分布）。
#   复判（人确认系统判得对不对：误报、漏检）。
# 还没有：批次、报告、SPC。
vision_domain_library(quality SOURCES domain/src/inspection_record.cpp domain/src/review.cpp)
vision_application_library(quality SOURCES application/src/quality_service.cpp)
