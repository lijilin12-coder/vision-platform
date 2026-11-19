// 计算图：把“网络长什么样”表示成数据，再把它编译成执行计划、按计划执行。
//
//   Graph    一组节点。每个节点写明自己是什么算子、输入来自哪些节点。节点的先后顺序没有意义。
//   compile  检查这张图，排出执行顺序，推导每个张量的形状，得到 Plan。只在加载模型时做一次。
//   run      按 Plan 逐步执行。每次推理都做。
//
// 和项目里 core/contexts/execution/domain 的 PlanSpec → ExecutionPlanner → Executor 是同一个结构。
#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../lesson01/ops.h"

namespace inftut {

// ---------- 图 ----------

enum class OpType { Input, Constant, Conv, BatchNorm, Relu, MaxPool, Add, Linear, Softmax };

inline const char* opName(OpType op) {
    switch (op) {
        case OpType::Input: return "Input";
        case OpType::Constant: return "Constant";
        case OpType::Conv: return "Conv";
        case OpType::BatchNorm: return "BatchNorm";
        case OpType::Relu: return "Relu";
        case OpType::MaxPool: return "MaxPool";
        case OpType::Add: return "Add";
        case OpType::Linear: return "Linear";
        case OpType::Softmax: return "Softmax";
    }
    return "?";
}

struct Shape {
    int n = 0, c = 0, h = 0, w = 0;
    bool operator==(const Shape& o) const { return n == o.n && c == o.c && h == o.h && w == o.w; }
    bool operator!=(const Shape& o) const { return !(*this == o); }
    std::size_t elements() const {
        return static_cast<std::size_t>(n) * static_cast<std::size_t>(c) * static_cast<std::size_t>(h) *
               static_cast<std::size_t>(w);
    }
    std::string text() const {
        return std::to_string(n) + "x" + std::to_string(c) + "x" + std::to_string(h) + "x" + std::to_string(w);
    }
};

inline Shape shapeOf(const Tensor& t) { return Shape{t.n, t.c, t.h, t.w}; }

// 一个节点 = 一次算子调用。为了简单，每个节点只有一个输出，输出张量的名字就是节点的名字。
struct Node {
    std::string name;
    OpType op = OpType::Input;
    std::vector<std::string> inputs;  // 输入来自哪些节点（按算子要求的顺序）

    // 属性：算子的参数里不是张量的那部分
    int stride = 1;      // Conv
    int pad = 0;         // Conv
    int kernel = 2;      // MaxPool
    bool relu = false;   // Conv：算完顺手做 ReLU（第 3 课的算子融合会把它设成 true）
    Shape inputShape;    // Input：这个模型接受什么形状的输入

    // 权重：算子的参数里是张量的那部分。它们是模型文件的主体。
    //   Conv、Linear：{卷积核或权重矩阵, 偏置}
    //   BatchNorm：  {gamma, beta, mean, var}
    //   Constant：   {它的值}
    std::vector<Tensor> weights;
};

struct Graph {
    std::vector<Node> nodes;
    std::string output;  // 哪个节点的输出是整个模型的输出
};

// 下面几个函数只是为了让“往图里加节点”写起来短一些。
inline Node inputNode(std::string name, Shape shape) {
    Node n;
    n.name = std::move(name);
    n.op = OpType::Input;
    n.inputShape = shape;
    return n;
}
inline Node constantNode(std::string name, Tensor value) {
    Node n;
    n.name = std::move(name);
    n.op = OpType::Constant;
    n.weights = {std::move(value)};
    return n;
}
inline Node convNode(std::string name, std::string in, Tensor weight, Tensor bias, int stride, int pad) {
    Node n;
    n.name = std::move(name);
    n.op = OpType::Conv;
    n.inputs = {std::move(in)};
    n.stride = stride;
    n.pad = pad;
    n.weights = {std::move(weight), std::move(bias)};
    return n;
}
inline Node batchNormNode(std::string name, std::string in, Tensor gamma, Tensor beta, Tensor mean, Tensor var) {
    Node n;
    n.name = std::move(name);
    n.op = OpType::BatchNorm;
    n.inputs = {std::move(in)};
    n.weights = {std::move(gamma), std::move(beta), std::move(mean), std::move(var)};
    return n;
}
inline Node simpleNode(std::string name, OpType op, std::vector<std::string> inputs) {  // Relu、Add、Softmax
    Node n;
    n.name = std::move(name);
    n.op = op;
    n.inputs = std::move(inputs);
    return n;
}
inline Node maxPoolNode(std::string name, std::string in, int kernel) {
    Node n;
    n.name = std::move(name);
    n.op = OpType::MaxPool;
    n.inputs = {std::move(in)};
    n.kernel = kernel;
    return n;
}
inline Node linearNode(std::string name, std::string in, Tensor weight, Tensor bias) {
    Node n;
    n.name = std::move(name);
    n.op = OpType::Linear;
    n.inputs = {std::move(in)};
    n.weights = {std::move(weight), std::move(bias)};
    return n;
}

// ---------- 形状推导 ----------

// 每种算子需要几个输入
inline std::size_t arity(OpType op) {
    switch (op) {
        case OpType::Input:
        case OpType::Constant: return 0;
        case OpType::Add: return 2;
        default: return 1;
    }
}

// 已知输入的形状，算出输出的形状；顺便检查这个节点合不合法。
// 不合法时返回 false，并把原因写进 error。
inline bool inferShape(const Node& node, const std::vector<Shape>& in, Shape& out, std::string& error) {
    auto fail = [&](const std::string& why) {
        error = "节点 " + node.name + "（" + opName(node.op) + "）：" + why;
        return false;
    };
    if (in.size() != arity(node.op)) return fail("需要 " + std::to_string(arity(node.op)) + " 个输入，实际有 " + std::to_string(in.size()) + " 个");

    switch (node.op) {
        case OpType::Input:
            out = node.inputShape;
            return out.elements() > 0 ? true : fail("没有指定输入形状");
        case OpType::Constant:
            if (node.weights.size() != 1) return fail("常量节点必须带一个张量");
            out = shapeOf(node.weights[0]);
            return true;
        case OpType::Conv: {
            if (node.weights.size() != 2) return fail("需要卷积核和偏置两个张量");
            const Tensor& w = node.weights[0];
            if (w.c != in[0].c) return fail("卷积核要求 " + std::to_string(w.c) + " 个输入通道，输入是 " + in[0].text());
            if (node.weights[1].c != w.n) return fail("偏置的个数和输出通道数不一致");
            const int oh = (in[0].h + 2 * node.pad - w.h) / node.stride + 1;
            const int ow = (in[0].w + 2 * node.pad - w.w) / node.stride + 1;
            if (oh <= 0 || ow <= 0) return fail("输入 " + in[0].text() + " 比卷积核还小");
            out = Shape{in[0].n, w.n, oh, ow};
            return true;
        }
        case OpType::BatchNorm:
            if (node.weights.size() != 4) return fail("需要 gamma、beta、mean、var 四个张量");
            for (const Tensor& t : node.weights)
                if (t.c != in[0].c) return fail("参数的通道数和输入 " + in[0].text() + " 不一致");
            out = in[0];
            return true;
        case OpType::Relu:
            out = in[0];
            return true;
        case OpType::MaxPool:
            if (node.kernel < 1 || in[0].h < node.kernel || in[0].w < node.kernel) return fail("输入 " + in[0].text() + " 比池化窗口还小");
            out = Shape{in[0].n, in[0].c, in[0].h / node.kernel, in[0].w / node.kernel};
            return true;
        case OpType::Add:
            if (in[0] != in[1]) return fail("两个输入的形状不同：" + in[0].text() + " 和 " + in[1].text());
            out = in[0];
            return true;
        case OpType::Linear: {
            if (node.weights.size() != 2) return fail("需要权重和偏置两个张量");
            const int features = in[0].c * in[0].h * in[0].w;
            if (node.weights[0].c != features) return fail("权重要求 " + std::to_string(node.weights[0].c) + " 个输入，输入 " + in[0].text() + " 有 " + std::to_string(features) + " 个");
            out = Shape{in[0].n, node.weights[0].n, 1, 1};
            return true;
        }
        case OpType::Softmax:
            if (in[0].h != 1 || in[0].w != 1) return fail("输入必须是 Nx类别数x1x1，实际是 " + in[0].text());
            out = in[0];
            return true;
    }
    return fail("未知的算子");
}

// ---------- 执行计划 ----------

struct Step {
    Node node;                        // 这一步执行哪个节点
    std::vector<std::size_t> inputs;  // 每个输入来自第几步的输出（一定比自己的下标小）
    Shape shape;                      // 这一步输出的形状
};

struct Plan {
    std::vector<Step> steps;  // 已经排好了顺序：从头到尾执行即可
    std::size_t inputStep = 0;
    std::size_t outputStep = 0;
};

struct CompileResult {
    std::optional<Plan> plan;  // 成功时有值
    std::string error;         // 失败时是原因
};

// 把图编译成执行计划：检查 → 拓扑排序 → 绑定输入 → 推导形状。
inline CompileResult compile(const Graph& graph) {
    auto fail = [](const std::string& why) { return CompileResult{std::nullopt, why}; };
    const std::size_t count = graph.nodes.size();

    // 1. 名字 → 下标。名字不能重复。
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < count; ++i)
        if (!index.emplace(graph.nodes[i].name, i).second) return fail("节点名字重复：" + graph.nodes[i].name);

    // 2. 建立“谁在我后面”的表，并数出每个节点有几个还没算出来的输入（入度）
    std::vector<std::vector<std::size_t>> next(count);
    std::vector<int> indegree(count, 0);
    for (std::size_t i = 0; i < count; ++i)
        for (const std::string& from : graph.nodes[i].inputs) {
            const auto it = index.find(from);
            if (it == index.end()) return fail("节点 " + graph.nodes[i].name + " 的输入 " + from + " 不存在");
            next[it->second].push_back(i);
            ++indegree[i];
        }

    // 3. 拓扑排序（Kahn 算法）：反复取出“输入都已经算出来了”的节点
    std::deque<std::size_t> ready;
    for (std::size_t i = 0; i < count; ++i)
        if (indegree[i] == 0) ready.push_back(i);
    std::vector<std::size_t> order;
    while (!ready.empty()) {
        const std::size_t cur = ready.front();
        ready.pop_front();
        order.push_back(cur);
        for (std::size_t nx : next[cur])
            if (--indegree[nx] == 0) ready.push_back(nx);
    }
    if (order.size() != count) return fail("图里有环，排不出执行顺序");

    // 4. 生成步骤：把“输入的名字”换成“第几步”，并推导形状
    std::vector<std::size_t> stepOf(count, 0);  // 节点下标 → 步骤下标
    for (std::size_t s = 0; s < count; ++s) stepOf[order[s]] = s;

    Plan plan;
    plan.steps.reserve(count);
    bool hasInput = false;
    for (std::size_t s = 0; s < count; ++s) {
        Step step;
        step.node = graph.nodes[order[s]];
        std::vector<Shape> inShapes;
        for (const std::string& from : step.node.inputs) {
            const std::size_t src = stepOf[index[from]];
            step.inputs.push_back(src);
            inShapes.push_back(plan.steps[src].shape);  // 上游一定排在前面，形状已经算出来了
        }
        std::string error;
        if (!inferShape(step.node, inShapes, step.shape, error)) return fail(error);
        if (step.node.op == OpType::Input) {
            if (hasInput) return fail("只支持一个 Input 节点");
            hasInput = true;
            plan.inputStep = s;
        }
        plan.steps.push_back(std::move(step));
    }
    if (!hasInput) return fail("图里没有 Input 节点");

    const auto out = index.find(graph.output);
    if (out == index.end()) return fail("输出节点 " + graph.output + " 不存在");
    plan.outputStep = stepOf[out->second];
    return CompileResult{std::move(plan), ""};
}

// ---------- 执行 ----------

// 执行一个节点：按算子类型调用第 1 课的函数。Input 和 Constant 不在这里处理。
// 对 Relu、Add、BatchNorm、Softmax，out 可以和 in[0] 是同一个张量（原地，第 4 课用）。
inline void runNode(const Node& node, const std::vector<const Tensor*>& in, Tensor& out) {
    switch (node.op) {
        case OpType::Conv:
            conv2dInto(*in[0], node.weights[0], node.weights[1], node.stride, node.pad, node.relu, out);
            break;
        case OpType::BatchNorm:
            batchNormInto(*in[0], node.weights[0], node.weights[1], node.weights[2], node.weights[3], out);
            break;
        case OpType::Relu: reluInto(*in[0], out); break;
        case OpType::MaxPool: maxPoolInto(*in[0], node.kernel, out); break;
        case OpType::Add: addInto(*in[0], *in[1], out); break;
        case OpType::Linear: linearInto(*in[0], node.weights[0], node.weights[1], out); break;
        case OpType::Softmax: softmaxInto(*in[0], out); break;
        case OpType::Input:
        case OpType::Constant: assert(false && "Input 和 Constant 由执行器处理"); break;
    }
}

// 按计划执行一次推理。stepMs 不为空时，记录每一步的耗时（毫秒）。
// 这是最简单的执行器：每一步的输出都放在一个新申请的张量里，全部留到推理结束。第 4 课会改进它。
inline Tensor run(const Plan& plan, const Tensor& input, std::vector<double>* stepMs = nullptr) {
    using Clock = std::chrono::steady_clock;
    assert(shapeOf(input) == plan.steps[plan.inputStep].shape);
    std::vector<Tensor> values(plan.steps.size());  // values[i] 是第 i 步的输出
    if (stepMs) stepMs->assign(plan.steps.size(), 0.0);

    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        const Step& step = plan.steps[i];
        const auto start = Clock::now();
        if (step.node.op == OpType::Input) {
            values[i] = input;
        } else if (step.node.op == OpType::Constant) {
            values[i] = step.node.weights[0];
        } else {
            std::vector<const Tensor*> in;
            for (std::size_t src : step.inputs) in.push_back(&values[src]);
            runNode(step.node, in, values[i]);
        }
        if (stepMs) (*stepMs)[i] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    return std::move(values[plan.outputStep]);
}

}  // namespace inftut
