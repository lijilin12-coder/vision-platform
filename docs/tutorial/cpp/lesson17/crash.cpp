// 崩溃转储练习：这个程序会崩溃。【故意】有错，用来练习“从转储文件里找出崩溃原因”。
//
// macOS / Linux：
//     clang++ -std=c++17 -g -O0 crash.cpp -o crash
//     ulimit -c unlimited        # 允许生成 core 文件（只对当前终端有效）
//     ./crash
// Windows：
//     cl /std:c++17 /EHsc /utf-8 /Zi /Od crash.cpp
//     crash.exe                  # 在 Visual Studio 里按 F5 运行，崩溃时会直接停在出错的那一行
#include <iostream>
#include <string>
#include <vector>

struct Blob {
    int id;
    double area;
};

struct InspectionContext {
    std::string recipe;
    const std::vector<Blob>* blobs = nullptr;  // 上游节点的输出；上游失败时为空
};

// 找出面积最大的连通块。
const Blob& largestBlob(const InspectionContext& ctx) {
    const std::vector<Blob>& blobs = *ctx.blobs;  // ← 没有检查 ctx.blobs 是不是空指针
    std::size_t best = 0;
    for (std::size_t i = 1; i < blobs.size(); ++i) {
        if (blobs[i].area > blobs[best].area) best = i;
    }
    return blobs[best];
}

double judge(const InspectionContext& ctx, double maxArea) {
    const Blob& blob = largestBlob(ctx);
    return blob.area > maxArea ? blob.area - maxArea : 0.0;
}

void runInspection(int frame, bool upstreamFailed) {
    const std::vector<Blob> found{{1, 12.5}, {2, 80.0}};
    InspectionContext ctx;
    ctx.recipe = "bright-spot@v1";
    if (!upstreamFailed) ctx.blobs = &found;

    std::cerr << "frame " << frame << ": 开始判定\n";  // cerr 不带缓冲：崩溃前一定能输出
    const double excess = judge(ctx, 50.0);
    std::cerr << "frame " << frame << ": 超出 " << excess << "\n";
}

int main() {
    runInspection(1, false);
    runInspection(2, false);
    runInspection(3, true);  // 第 3 帧：上游节点失败了
    runInspection(4, false);
    return 0;
}
