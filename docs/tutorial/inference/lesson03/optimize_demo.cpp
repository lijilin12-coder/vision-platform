// 图优化：对一张“刚导出、没整理过”的图运行四个遍，检查结果不变，比较优化前后的耗时。
//
//   optimize_demo            小尺寸（32×32，8 通道），主要看每个遍做了什么、结果对不对
//   optimize_demo 64 16      大一些的尺寸（64×64，16 通道），用优化模式编译后看耗时
//
//   macOS / Linux：clang++ -std=c++17 -O2 -I../common optimize_demo.cpp -o optimize_demo && ./optimize_demo 64 16
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 /I..\common optimize_demo.cpp && optimize_demo.exe 64 16
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "../lesson02/model.h"
#include "passes.h"

using namespace inftut;

namespace {

volatile float sink = 0;  // 防止计时的时候计算被优化掉

// 在第 2 课的网络上，加上导出的模型里常见的两类“多余的东西”。
Graph buildExportedModel(int size, int channels) {
    Graph g = buildSmallResNet(size, channels);
    Rng rng(7);

    // (1) 预处理被原样写进了图里：norm = input + (negMean + shift)。
    //     negMean 和 shift 都是常量，它们的和现在就能算出来。
    g.nodes.push_back(constantNode("neg_mean", randomTensor(rng, 1, 3, size, size, 0.1f)));
    g.nodes.push_back(constantNode("shift", randomTensor(rng, 1, 3, size, size, 0.1f)));
    g.nodes.push_back(simpleNode("offset", OpType::Add, {"neg_mean", "shift"}));
    g.nodes.push_back(simpleNode("norm", OpType::Add, {"input", "offset"}));
    for (Node& n : g.nodes)
        if (n.name == "conv1") n.inputs = {"norm"};

    // (2) 训练时用的辅助输出没有删掉：一个卷积加一个 ReLU，结果没有人用。
    addConv(g, rng, "aux_conv", "pool1", channels, channels);
    g.nodes.push_back(simpleNode("aux_relu", OpType::Relu, {"aux_conv"}));
    return g;
}

Plan mustCompile(const Graph& g) {
    CompileResult r = compile(g);
    if (!r.plan) {
        std::printf("编译失败：%s\n", r.error.c_str());
        std::exit(1);
    }
    return std::move(*r.plan);
}

std::string countByOp(const Graph& g) {
    std::map<std::string, int> counts;
    for (const Node& n : g.nodes) ++counts[n.op == OpType::Conv && n.relu ? "Conv+ReLU" : opName(n.op)];
    std::string text;
    for (const auto& [op, count] : counts) text += (text.empty() ? "" : "，") + op + " " + std::to_string(count);
    return text;
}

// 把每一步的耗时按算子类型加起来。每一步重复多次取中位数。
std::map<std::string, double> timeByOp(const Plan& plan, const Tensor& image, int repeats, double& totalMs) {
    std::vector<std::vector<double>> samples(plan.steps.size());
    std::vector<double> stepMs;
    for (int r = 0; r < repeats; ++r) {
        const Tensor out = run(plan, image, &stepMs);
        sink = out.data[0];
        for (std::size_t i = 0; i < stepMs.size(); ++i) samples[i].push_back(stepMs[i]);
    }
    std::map<std::string, double> byOp;
    totalMs = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        std::sort(samples[i].begin(), samples[i].end());
        const double median = samples[i][samples[i].size() / 2];
        byOp[opName(plan.steps[i].node.op)] += median;
        totalMs += median;
    }
    return byOp;
}

}  // namespace

int main(int argc, char** argv) {
    const int size = argc > 1 ? std::atoi(argv[1]) : 32;
    const int channels = argc > 2 ? std::atoi(argv[2]) : 8;
    if (size < 8 || size % 8 != 0 || channels < 1) {
        std::printf("用法：optimize_demo [图像边长（8 的倍数）] [通道数]\n");
        return 1;
    }

    const Graph original = buildExportedModel(size, channels);
    const Tensor image = randomImage(size);
    const Plan planBefore = mustCompile(original);
    const Tensor expected = run(planBefore, image);

    std::printf("输入 %dx%d，第一层 %d 个通道\n\n", size, size, channels);
    std::printf("优化前：%zu 个节点（%s）\n", original.nodes.size(), countByOp(original).c_str());

    // 运行全部的遍。每个遍改动之后，立刻重新编译、执行，检查输出和优化前相比有没有变。
    bool ok = true;
    Graph optimized = original;
    optimize(optimized, [&](const Pass& pass, int changes, const Graph& g) {
        const double diff = maxAbsDiff(run(mustCompile(g), image).data, expected.data);
        std::printf("  改动 %d 处，剩 %2zu 个节点，输出的最大变化 %-8.2g ← %s\n", changes, g.nodes.size(), diff, pass.name);
        ok = ok && diff < 1e-5;
    });
    std::printf("优化后：%zu 个节点（%s）\n", optimized.nodes.size(), countByOp(optimized).c_str());

    const Plan planAfter = mustCompile(optimized);
    std::printf("\n优化后的执行顺序：");
    for (const Step& s : planAfter.steps) std::printf("%s%s ", s.node.name.c_str(), s.node.relu ? "(+relu)" : "");
    std::printf("\n");

    // 耗时对比
    const int repeats = 15;
    double totalBefore = 0;
    double totalAfter = 0;
    const std::map<std::string, double> before = timeByOp(planBefore, image, repeats, totalBefore);
    std::map<std::string, double> after = timeByOp(planAfter, image, repeats, totalAfter);
    std::printf("\n按算子类型统计的耗时（毫秒，每一步重复 %d 次取中位数）：\n", repeats);
    std::printf("  %-10s %9s %9s\n", "", "before", "after");
    for (const auto& [op, ms] : before) std::printf("  %-10s %9.3f %9.3f\n", op.c_str(), ms, after[op]);
    std::printf("  %-10s %9.3f %9.3f   （快了 %.1f%%）\n", "total", totalBefore, totalAfter,
                100 * (totalBefore - totalAfter) / totalBefore);

    // 自检：融合应该恰好去掉全部 4 个 BatchNorm、3 个 ReLU（relu2 在 Add 后面，去不掉）
    int batchNorms = 0;
    int relus = 0;
    int constants = 0;
    for (const Node& n : optimized.nodes) {
        if (n.op == OpType::BatchNorm) ++batchNorms;
        if (n.op == OpType::Relu) ++relus;
        if (n.op == OpType::Constant) ++constants;
    }
    ok = ok && batchNorms == 0 && relus == 1 && constants == 1 && optimized.nodes.size() == 14;
    std::printf("\n%s\n", ok ? "检查通过：优化前后输出一致" : "检查没有通过");
    return ok ? 0 : 1;
}
