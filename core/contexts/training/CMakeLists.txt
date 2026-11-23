# BC-12 模型训练（支撑域）。训练任务的生命周期；真正的训练由可替换的“训练后端”完成（ADR-30）。
# 应用层不依赖其他上下文：数据集版本、模型仓库都通过它自己定义的端口取得。
vision_domain_library(training SOURCES domain/src/training_job.cpp)
vision_application_library(training SOURCES application/src/training_service.cpp)
