# 第 06 课：量化——从 FP32 到 INT8
inftut_example(06 quant_basics SOURCES quant_basics.cpp)                 # 概念：带内存检查工具
inftut_example(06 quant_mlp SOURCES quant_mlp.cpp OPTIMIZE)              # 训练一个小模型再量化：打开优化，否则训练太慢
inftut_example(06 quant_speed SOURCES quant_speed.cpp OPTIMIZE NATIVE)   # 测量时间
