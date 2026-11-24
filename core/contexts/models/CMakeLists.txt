# BC-7 模型仓库（支撑域）。目前是训练闭环（POC-13）需要的最小部分：
#   模型版本（带模型契约和来源）、导入、发布。
# 还没有：模型制品的多平台格式、量化、转换、验证（BC-8 模型工具链，POC-10）。
vision_domain_library(models SOURCES domain/src/model_version.cpp)
vision_application_library(models SOURCES application/src/model_service.cpp)
