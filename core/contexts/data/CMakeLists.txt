# BC-9 数据（支撑域）。目前是训练闭环（POC-13）需要的最小部分：
#   样本（按图像内容识别）、样本用途和规则 N1、数据集和冻结的数据集版本。
# 还没有：采集策略、短期图像缓存、标注（检测框、分割）、样本包。
vision_domain_library(data SOURCES
  domain/src/sample.cpp
  domain/src/dataset.cpp)

vision_application_library(data SOURCES application/src/data_service.cpp)
