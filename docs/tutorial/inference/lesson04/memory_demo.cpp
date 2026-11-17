// 内存规划：算出每个张量的生命周期，让生命周期不重叠的张量共用缓冲区。
//
//   memory_demo            小尺寸（32×32，8 通道）
//   memory_demo 64 16      大一些的尺寸，用优化模式编译后看耗时
//
//   macOS / Linux：clang++ -std=c++17 -O2 -I../common memory_demo.cpp -o memory_demo && ./memory_demo 64 16
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 /I..\common memory_demo.cpp && memory_demo.exe 64 16
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../lesson02/model.h"
#include "../lesson03/passes.h"
#include "memory_plan.h"

using namespace inftut;

namespace {

volatile float sink = 0;  // 防止计时的时候计算被优化掉

Plan mustCompile(const Graph& g) {
    CompileResult r = compile(g);
    if (!r.plan) {
        std::printf("编译失败：%s\n", r.error.c_str());
        std::exit(1);
    }
    return std::move(*r.plan);
}

double kb(std::size_t elements) { return static_cast<double>(elements * sizeof(float)) / 1024.0; }

// 画出每个张量的生命周期，以及两种规划各自把它放在哪块缓冲区。
void printLifetimes(const Plan& plan, const std::vector<std::size_t>& lastUse, const MemoryPlan& noInPlace,
                    const MemoryPlan& withInPlace) {
    const std::size_t count = plan.steps.size();
    std::printf("  step name    %8s  lifetime%*s  buffer  buffer(in-place)\n", "KB", static_cast<int>(count) - 8, "");
    for (std::size_t i = 0; i < count; ++i) {
        const Step& s = plan.steps[i];
        if (!needsBuffer(s)) continue;
        std::string bar(count, '.');
        for (std::size_t t = i; t <= lastUse[i] && t < count; ++t) bar[t] = '#';
        std::printf("  #%-3zu %-7s %8.1f  %s  %-6c  %c%s\n", i, s.node.name.c_str(), kb(s.shape.elements()), bar.c_str(),
                    static_cast<char>('A' + noInPlace.bufferOf[i]), static_cast<char>('A' + withInPlace.bufferOf[i]),
                    withInPlace.inPlace[i] ? " (in-place)" : "");
    }
}

void printBuffers(const char* title, const MemoryPlan& mem) {
    std::printf("  %s：%zu 块缓冲区，共 %.1f KB（", title, mem.bufferSize.size(), kb(mem.totalElements()));
    for (std::size_t b = 0; b < mem.bufferSize.size(); ++b)
        std::printf("%s%c %.1f", b ? "，" : "", static_cast<char>('A' + b), kb(mem.bufferSize[b]));
    std::printf("）\n");
}

}  // namespace

int main(int argc, char** argv) {
    const int size = argc > 1 ? std::atoi(argv[1]) : 32;
    const int channels = argc > 2 ? std::atoi(argv[2]) : 8;
    if (size < 8 || size % 8 != 0 || channels < 1) {
        std::printf("用法：memory_demo [图像边长（8 的倍数）] [通道数]\n");
        return 1;
    }
    bool ok = true;

    const Graph graph = buildSmallResNet(size, channels);
    const Tensor image = randomImage(size);
    const Plan plan = mustCompile(graph);
    const Tensor expected = run(plan, image);  // 第 2 课的执行器：每个张量一块新内存

    // ---- 1. 生命周期和规划 ----
    const std::vector<std::size_t> lastUse = computeLastUse(plan);
    const MemoryPlan noInPlace = planMemory(plan, lastUse, false);
    const MemoryPlan withInPlace = planMemory(plan, lastUse, true);

    std::printf("输入 %dx%d，第一层 %d 个通道\n\n", size, size, channels);
    std::printf("1. 每个中间张量的生命周期（# 表示这一步时它必须存在），和分到的缓冲区：\n");
    printLifetimes(plan, lastUse, noInPlace, withInPlace);

    const std::size_t naive = totalIntermediateElements(plan);
    const std::size_t peak = peakLiveElements(plan, lastUse);
    std::printf("\n2. 中间张量占用的内存：\n");
    std::printf("  不规划（每个张量一块，留到最后）：%zu 块，共 %.1f KB\n", noInPlace.bufferOf.size() - 1, kb(naive));
    printBuffers("规划，不允许原地", noInPlace);
    printBuffers("规划，允许原地  ", withInPlace);
    std::printf("  同一时刻必须存在的张量最多 %.1f KB（不做原地时的下界）\n", kb(peak));
    std::printf("  规划后是不规划的 %.0f%%（不原地）和 %.0f%%（原地）\n",
                100.0 * static_cast<double>(noInPlace.totalElements()) / static_cast<double>(naive),
                100.0 * static_cast<double>(withInPlace.totalElements()) / static_cast<double>(naive));
    ok = ok && noInPlace.totalElements() >= peak && withInPlace.totalElements() <= noInPlace.totalElements();

    // ---- 2. 按规划执行：结果必须和原来完全相同，而且推理时不申请内存 ----
    PlannedExecutor executor(plan, withInPlace);
    const std::vector<const float*> before = executor.bufferAddresses();
    double diff = 0;
    for (int i = 0; i < 3; ++i) diff = std::max(diff, maxAbsDiff(executor.run(image).data, expected.data));
    const bool sameAddresses = before == executor.bufferAddresses();
    std::printf("\n3. 按规划（原地）执行 3 次：和第 2 课执行器的最大差距 %g；缓冲区的地址%s\n", diff,
                sameAddresses ? "没有变（没有重新申请过内存）" : "变了（重新申请了内存）");
    ok = ok && diff == 0.0 && sameAddresses;

    PlannedExecutor executorNoInPlace(plan, noInPlace);
    ok = ok && maxAbsDiff(executorNoInPlace.run(image).data, expected.data) == 0.0;

    // ---- 3. 生命周期算错了会怎样 ----
    // pool1 的输出被用了两次：conv2a 和后面的 add2。假装只看到了第一次。
    std::vector<std::size_t> wrongLastUse = lastUse;
    for (std::size_t i = 0; i < plan.steps.size(); ++i)
        if (plan.steps[i].node.name == "pool1") wrongLastUse[i] = i + 1;
    PlannedExecutor broken(plan, planMemory(plan, wrongLastUse, true));
    const double brokenDiff = maxAbsDiff(broken.run(image).data, expected.data);
    std::printf("\n4. 故意把 pool1 的生命周期算短（漏掉 add2 对它的使用）：输出和正确结果的最大差距 %.3f\n", brokenDiff);
    std::printf("   程序没有崩溃，内存检查工具也不报错，只是结果错了。\n");
    ok = ok && brokenDiff > 1e-3;

    // ---- 4. 和第 3 课的图优化叠加 ----
    Graph optimizedGraph = graph;
    optimize(optimizedGraph);
    const Plan optimizedPlan = mustCompile(optimizedGraph);
    const MemoryPlan optimizedMem = planMemory(optimizedPlan, computeLastUse(optimizedPlan), true);
    PlannedExecutor optimizedExecutor(optimizedPlan, optimizedMem);
    const double optimizedDiff = maxAbsDiff(optimizedExecutor.run(image).data, expected.data);
    std::printf("\n5. 先做第 3 课的图优化，再规划：\n");
    std::printf("  不规划：%.1f KB\n", kb(totalIntermediateElements(optimizedPlan)));
    printBuffers("规划，允许原地  ", optimizedMem);
    std::printf("  输出和优化前的最大差距 %.2g\n", optimizedDiff);
    ok = ok && optimizedDiff < 1e-5;

    // ---- 5. 耗时 ----
    const int repeats = 15;
    const double msNaive = medianMs(repeats, [&] { sink = run(plan, image).data[0]; });
    const double msPlanned = medianMs(repeats, [&] { sink = executor.run(image).data[0]; });
    const double msBoth = medianMs(repeats, [&] { sink = optimizedExecutor.run(image).data[0]; });
    // 单独测量“申请”这件事本身：把第 2 课执行器每次推理要申请的那些张量申请一遍（会清零），再释放
    const double msAlloc = medianMs(repeats, [&] {
        std::vector<Tensor> values(plan.steps.size());
        for (std::size_t i = 0; i < plan.steps.size(); ++i) {
            const Shape& s = plan.steps[i].shape;
            values[i].resize(s.n, s.c, s.h, s.w);
            sink = values[i].data[0];
        }
    });
    std::printf("\n6. 一次推理的耗时（重复 %d 次取中位数）：\n", repeats);
    std::printf("  第 2 课的执行器        %8.3f ms\n", msNaive);
    std::printf("  按规划执行             %8.3f ms\n", msPlanned);
    std::printf("  图优化 + 按规划执行    %8.3f ms\n", msBoth);
    std::printf("  （只申请、清零、释放第 2 课执行器用到的全部张量：%.3f ms）\n", msAlloc);

    std::printf("\n%s\n", ok ? "全部检查通过" : "有检查没有通过");
    return ok ? 0 : 1;
}
