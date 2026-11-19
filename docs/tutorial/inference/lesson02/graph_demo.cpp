// 计算图 → 执行计划 → 执行。
//
//   1. 把示例网络编译成执行计划，打印每一步
//   2. 按计划执行，和“直接手写前向计算”的结果比较，必须完全相同
//   3. 把图里的节点倒过来放，再编译执行：结果不变（节点的存放顺序没有意义）
//   4. 三张有问题的图：有环、引用了不存在的节点、形状对不上。编译时就要报出来
//
//   macOS / Linux：clang++ -std=c++17 -I../common graph_demo.cpp -o graph_demo && ./graph_demo
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common graph_demo.cpp && graph_demo.exe
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "graph.h"
#include "model.h"

using namespace inftut;

namespace {

const Node& find(const Graph& g, const char* name) {
    for (const Node& n : g.nodes)
        if (n.name == name) return n;
    std::printf("找不到节点 %s\n", name);
    std::abort();
}

// 不用图，直接按网络结构手写前向计算。用来验证执行器算得对不对。
Tensor forwardByHand(const Graph& g, const Tensor& image) {
    auto conv = [&](const char* name, const Tensor& x) {
        const Node& n = find(g, name);
        return conv2d(x, n.weights[0], n.weights[1], n.stride, n.pad);
    };
    auto bn = [&](const char* name, const Tensor& x) {
        const Node& n = find(g, name);
        return batchNorm(x, n.weights[0], n.weights[1], n.weights[2], n.weights[3]);
    };
    const Tensor pool1 = maxPool(relu(bn("bn1", conv("conv1", image))), 2);
    const Tensor branch = bn("bn2b", conv("conv2b", relu(bn("bn2a", conv("conv2a", pool1)))));
    const Tensor pool2 = maxPool(relu(add(branch, pool1)), 2);
    const Tensor pool3 = maxPool(relu(bn("bn3", conv("conv3", pool2))), 2);
    const Node& fc = find(g, "fc");
    return softmax(linear(pool3, fc.weights[0], fc.weights[1]));
}

void printPlan(const Plan& plan) {
    std::printf("  步骤  节点      算子        输入来自        输出形状\n");
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        const Step& s = plan.steps[i];
        std::string from;
        for (std::size_t src : s.inputs) from += (from.empty() ? "" : ", ") + std::string("#") + std::to_string(src);
        std::printf("  #%-4zu %-9s %-11s %-15s %s\n", i, s.node.name.c_str(), opName(s.node.op), from.c_str(),
                    s.shape.text().c_str());
    }
}

// 期望编译失败，打印原因。编译居然成功了就算测试失败。
bool expectCompileError(const std::string& what, const Graph& g) {
    const CompileResult r = compile(g);
    if (r.plan) {
        std::printf("  %s：应该失败，却编译成功了\n", what.c_str());
        return false;
    }
    std::printf("  %s → %s\n", what.c_str(), r.error.c_str());
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    const Graph graph = buildSmallResNet();
    const Tensor image = randomImage(32);

    // ---- 1. 编译 ----
    const CompileResult compiled = compile(graph);
    if (!compiled.plan) {
        std::printf("编译失败：%s\n", compiled.error.c_str());
        return 1;
    }
    std::printf("1. 执行计划（%zu 步）：\n", compiled.plan->steps.size());
    printPlan(*compiled.plan);

    // ---- 2. 执行，并和手写的前向计算比较 ----
    const Tensor byPlan = run(*compiled.plan, image);
    const Tensor byHand = forwardByHand(graph, image);
    const double diff = maxAbsDiff(byPlan.data, byHand.data);
    std::printf("\n2. 按计划执行的输出：");
    for (float p : byPlan.data) std::printf("%.3f ", static_cast<double>(p));
    std::printf("\n   和手写前向计算的最大差距：%g\n", diff);
    ok = ok && diff == 0.0;

    // ---- 3. 节点倒着放 ----
    Graph reversed = graph;
    std::reverse(reversed.nodes.begin(), reversed.nodes.end());
    const CompileResult compiledReversed = compile(reversed);
    if (!compiledReversed.plan) {
        std::printf("倒序的图编译失败：%s\n", compiledReversed.error.c_str());
        return 1;
    }
    std::printf("\n3. 把节点倒过来放（第一个节点是 %s），排出的执行顺序：\n   ", reversed.nodes.front().name.c_str());
    for (const Step& s : compiledReversed.plan->steps) std::printf("%s ", s.node.name.c_str());
    const double diffReversed = maxAbsDiff(run(*compiledReversed.plan, image).data, byPlan.data);
    std::printf("\n   输出和原来的最大差距：%g\n", diffReversed);
    ok = ok && diffReversed == 0.0;

    // ---- 4. 有问题的图 ----
    std::printf("\n4. 有问题的图，编译时报错：\n");
    Graph cyclic = graph;  // 让 conv2a 的输入变成 add2：conv2a → ... → add2 → conv2a
    for (Node& n : cyclic.nodes)
        if (n.name == "conv2a") n.inputs = {"add2"};
    ok = expectCompileError("有环", cyclic) && ok;

    Graph dangling = graph;
    for (Node& n : dangling.nodes)
        if (n.name == "relu3") n.inputs = {"bn33"};  // 名字写错了
    ok = expectCompileError("名字写错", dangling) && ok;

    Graph mismatched = graph;  // 让 add2 的第二个输入变成 relu1：它还没有经过池化，大小是另一个输入的两倍
    for (Node& n : mismatched.nodes)
        if (n.name == "add2") n.inputs = {"bn2b", "relu1"};
    ok = expectCompileError("形状对不上", mismatched) && ok;

    std::printf("\n%s\n", ok ? "全部检查通过" : "有检查没有通过");
    return ok ? 0 : 1;
}
