# 第 04 课：内存规划
inftut_example(04 memory_demo SOURCES memory_demo.cpp)                          # 小尺寸，带内存检查：看对不对
inftut_example(04 memory_bench SOURCES memory_demo.cpp OPTIMIZE ARGS 64 16)     # 大尺寸，打开优化：看耗时
