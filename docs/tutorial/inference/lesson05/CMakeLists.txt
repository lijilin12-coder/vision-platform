# 第 05 课：高性能算子——矩阵乘法
inftut_example(05 matmul SOURCES matmul.cpp OPTIMIZE NATIVE)   # 测量时间：打开优化，允许用本机的 SIMD 指令
inftut_example(05 im2col SOURCES im2col.cpp)                   # 只验证正确性：带内存检查工具
inftut_example(05 knn SOURCES knn.cpp OPTIMIZE NATIVE)
