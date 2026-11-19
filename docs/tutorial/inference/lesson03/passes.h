// 图优化：四个“遍”（pass）。每个遍读入一张图，把它改写成算得更快、但结果相同的另一张图。
//
//   removeDeadNodes   删除对输出没有贡献的节点
//   foldConstants     常量折叠：输入全是常量的节点，现在就把它算出来
//   fuseConvBatchNorm 把 Conv 后面的 BatchNorm 合并进卷积的权重
//   fuseConvRelu      把 Conv 后面的 ReLU 合并进卷积
//
// 每个遍返回它改动了几处；optimize() 反复运行全部的遍，直到谁都没有可改的为止。
#pragma once

#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../lesson02/graph.h"

namespace inftut {

// ---------- 几个遍都要用的小工具 ----------

// 找到名字对应的节点下标；找不到返回 nodes.size()
inline std::size_t findNode(const Graph& g, const std::string& name) {
    for (std::size_t i = 0; i < g.nodes.size(); ++i)
        if (g.nodes[i].name == name) return i;
    return g.nodes.size();
}

// 一个节点的输出被用了几次（被别的节点当输入，或者是整个图的输出）
inline int useCount(const Graph& g, const std::string& name) {
    int count = g.output == name ? 1 : 0;
    for (const Node& n : g.nodes)
        for (const std::string& in : n.inputs)
            if (in == name) ++count;
    return count;
}

// 把所有“用 oldName 当输入”的地方改成用 newName
inline void redirect(Graph& g, const std::string& oldName, const std::string& newName) {
    for (Node& n : g.nodes)
        for (std::string& in : n.inputs)
            if (in == oldName) in = newName;
    if (g.output == oldName) g.output = newName;
}

// ---------- 遍 1：删除无用节点 ----------
// 从输出节点出发，沿着输入往回走，走得到的节点才是有用的。Input 节点总是保留。
inline int removeDeadNodes(Graph& g) {
    std::map<std::string, bool> alive;
    std::vector<std::string> todo = {g.output};
    while (!todo.empty()) {
        const std::string name = todo.back();
        todo.pop_back();
        if (alive[name]) continue;
        alive[name] = true;
        const std::size_t i = findNode(g, name);
        if (i == g.nodes.size()) continue;  // 引用了不存在的节点：留给 compile 去报错
        for (const std::string& in : g.nodes[i].inputs) todo.push_back(in);
    }
    std::vector<Node> kept;
    for (Node& n : g.nodes)
        if (alive[n.name] || n.op == OpType::Input) kept.push_back(std::move(n));
    const int removed = static_cast<int>(g.nodes.size() - kept.size());
    g.nodes = std::move(kept);
    return removed;
}

// ---------- 遍 2：常量折叠 ----------
// 一个节点的输入如果全是常量，它的输出也是常量：现在就算出来，把这个节点换成一个 Constant 节点。
// 原来的那些常量如果没有别人用了，会在下一轮被 removeDeadNodes 删掉。
inline int foldConstants(Graph& g) {
    int folded = 0;
    for (Node& node : g.nodes) {
        if (node.op == OpType::Input || node.op == OpType::Constant) continue;
        std::vector<const Tensor*> in;
        for (const std::string& name : node.inputs) {
            const std::size_t i = findNode(g, name);
            if (i == g.nodes.size() || g.nodes[i].op != OpType::Constant) break;
            in.push_back(&g.nodes[i].weights[0]);
        }
        if (in.size() != node.inputs.size()) continue;  // 有的输入不是常量

        Tensor value;
        runNode(node, in, value);  // 用和推理时完全相同的代码来算
        node = constantNode(node.name, std::move(value));
        ++folded;
    }
    return folded;
}

// ---------- 遍 3：Conv + BatchNorm 融合 ----------
// 推理时 BatchNorm 对每个通道做 y = s·x + t，其中 s = gamma / sqrt(var + eps)，t = beta - mean·s。
// 卷积的每个输出通道是 x = Σ(w·输入) + b。代进去：
//     y = s·(Σ(w·输入) + b) + t = Σ((s·w)·输入) + (s·b + t)
// 还是一个卷积，只是权重变成 s·w、偏置变成 s·b + t。所以把权重改掉，BatchNorm 节点就可以删除。
//
// 条件：Conv 的输出只被这一个 BatchNorm 使用。否则别的使用者会拿到被改过的结果。
inline int fuseConvBatchNorm(Graph& g) {
    int fused = 0;
    for (std::size_t b = 0; b < g.nodes.size(); ++b) {
        if (g.nodes[b].op != OpType::BatchNorm) continue;
        const std::size_t c = findNode(g, g.nodes[b].inputs[0]);
        if (c == g.nodes.size() || g.nodes[c].op != OpType::Conv) continue;
        if (g.nodes[c].relu) continue;  // 卷积后面已经带了 ReLU，顺序是 Conv → ReLU → BN，不能这样合并
        if (useCount(g, g.nodes[c].name) != 1) continue;

        const Node bn = g.nodes[b];  // 复制一份：下面要改图
        Node& conv = g.nodes[c];
        Tensor& weight = conv.weights[0];
        Tensor& bias = conv.weights[1];
        const std::size_t perChannel = weight.size() / static_cast<std::size_t>(weight.n);
        for (int oc = 0; oc < weight.n; ++oc) {
            const std::size_t ch = static_cast<std::size_t>(oc);
            const float s = bn.weights[0].data[ch] / std::sqrt(bn.weights[3].data[ch] + kBatchNormEps);
            const float t = bn.weights[1].data[ch] - bn.weights[2].data[ch] * s;
            for (std::size_t i = 0; i < perChannel; ++i) weight.data[ch * perChannel + i] *= s;
            bias.data[ch] = bias.data[ch] * s + t;
        }
        // 原来用 BatchNorm 输出的地方，改成直接用卷积的输出；然后删除 BatchNorm 节点
        const std::string convName = conv.name;
        redirect(g, bn.name, convName);
        g.nodes.erase(g.nodes.begin() + static_cast<std::ptrdiff_t>(b));
        --b;
        ++fused;
    }
    return fused;
}

// ---------- 遍 4：Conv + ReLU 融合 ----------
// 卷积算出每个输出元素后，顺手把负数改成 0（conv2dInto 的 relu 参数），省掉对整个张量的第二次遍历。
// 条件同上：Conv 的输出只被这一个 ReLU 使用。
inline int fuseConvRelu(Graph& g) {
    int fused = 0;
    for (std::size_t r = 0; r < g.nodes.size(); ++r) {
        if (g.nodes[r].op != OpType::Relu) continue;
        const std::size_t c = findNode(g, g.nodes[r].inputs[0]);
        if (c == g.nodes.size() || g.nodes[c].op != OpType::Conv) continue;
        if (useCount(g, g.nodes[c].name) != 1) continue;

        g.nodes[c].relu = true;
        const std::string reluName = g.nodes[r].name;
        const std::string convName = g.nodes[c].name;
        redirect(g, reluName, convName);
        g.nodes.erase(g.nodes.begin() + static_cast<std::ptrdiff_t>(r));
        --r;
        ++fused;
    }
    return fused;
}

// ---------- 遍管理器 ----------

struct Pass {
    const char* name;
    int (*apply)(Graph&);
};

inline const std::vector<Pass>& allPasses() {
    static const std::vector<Pass> passes = {
        {"删除无用节点", removeDeadNodes},
        {"常量折叠", foldConstants},
        {"Conv+BatchNorm 融合", fuseConvBatchNorm},
        {"Conv+ReLU 融合", fuseConvRelu},
    };
    return passes;
}

// 反复运行全部的遍，直到一整轮下来没有任何改动。
// 一个遍的结果可能给另一个遍创造机会（折叠之后原来的常量变成了无用节点；
// 融合掉 BatchNorm 之后 Conv 和 ReLU 才挨在一起），所以要循环。
// onPass 不为空时，每个遍做完改动后调用它一次（示例用它来打印和检查）。
inline void optimize(Graph& g, const std::function<void(const Pass&, int changes, const Graph&)>& onPass = nullptr) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Pass& pass : allPasses()) {
            const int changes = pass.apply(g);
            if (changes == 0) continue;
            changed = true;
            if (onPass) onPass(pass, changes, g);
        }
    }
}

}  // namespace inftut
