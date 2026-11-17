# 第 03 课：图优化
inftut_example(03 optimize_demo SOURCES optimize_demo.cpp)                           # 小尺寸，带内存检查：看对不对
inftut_example(03 optimize_bench SOURCES optimize_demo.cpp OPTIMIZE ARGS 64 16)      # 大尺寸，打开优化：看耗时
