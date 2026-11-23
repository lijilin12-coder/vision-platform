#include <deque>

#include "vision/execution/domain/plan.h"

namespace vision::execution {

std::vector<std::string> ExecutionPlan::nodeOrder() const {
    std::vector<std::string> order;
    order.reserve(steps_.size());
    for (const auto& s : steps_) order.push_back(s.node.id);
    return order;
}

Result<ExecutionPlan> ExecutionPlanner::plan(const PlanSpec& spec) const {
    using R = Result<ExecutionPlan>;
    const size_t n = spec.nodes.size();
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < n; ++i) {
        if (!index.emplace(spec.nodes[i].id, i).second) {
            return R::failure("execution.duplicate_node", "节点 ID 重复：" + spec.nodes[i].id);
        }
    }

    // 校验连线并建立邻接表
    std::vector<std::vector<size_t>> next(n);
    std::vector<int> indegree(n, 0);
    for (const auto& e : spec.edges) {
        auto f = index.find(e.fromNode);
        auto t = index.find(e.toNode);
        if (f == index.end() || t == index.end()) {
            return R::failure("execution.bad_edge", "连线引用了不存在的节点：" + e.fromNode + " → " + e.toNode);
        }
        const auto* out = spec.nodes[f->second].signature.findOutput(e.fromPort);
        const auto* in = spec.nodes[t->second].signature.findInput(e.toPort);
        if (!out || !in || !isCompatible(out->type, in->type)) {
            return R::failure("execution.bad_edge",
                              "连线端口不存在或类型不兼容：" + e.fromNode + "." + e.fromPort + " → " + e.toNode + "." +
                                  e.toPort);
        }
        next[f->second].push_back(t->second);
        ++indegree[t->second];
    }

    // Kahn 算法：反复取出入度为 0 的节点。保持原始顺序，使执行顺序稳定、可预期。
    std::deque<size_t> ready;
    for (size_t i = 0; i < n; ++i)
        if (indegree[i] == 0) ready.push_back(i);
    std::vector<size_t> order;
    while (!ready.empty()) {
        size_t cur = ready.front();
        ready.pop_front();
        order.push_back(cur);
        for (size_t nx : next[cur])
            if (--indegree[nx] == 0) ready.push_back(nx);
    }
    if (order.size() != n) return R::failure("execution.cycle", "方案中存在环，无法确定执行顺序");

    // 生成步骤
    std::map<size_t, size_t> stepOf;  // 节点下标 → 步骤下标
    for (size_t s = 0; s < order.size(); ++s) stepOf[order[s]] = s;

    std::vector<PlanStep> steps;
    steps.reserve(n);
    for (size_t nodeIdx : order) {
        const PlanNode& node = spec.nodes[nodeIdx];
        PlanStep step;
        step.node = node;
        step.isImageInput = node.signature.id == kImageInputOperatorId;
        for (const auto& e : spec.edges) {
            if (e.toNode == node.id) step.inputs[e.toPort] = InputBinding{stepOf[index[e.fromNode]], e.fromPort};
        }
        for (const auto& in : node.signature.inputs) {
            if (!in.optional && step.inputs.count(in.name) == 0) {
                return R::failure("execution.unbound_input", "节点 " + node.id + " 的必填输入 " + in.name + " 没有连接");
            }
        }
        if (!step.isImageInput) {
            auto runner = factory_.create(node);
            if (!runner) return runner.error();
            step.runner = runner.value();
        }
        steps.push_back(std::move(step));
    }
    return ExecutionPlan(spec.recipeVersionId, std::move(steps));
}

}  // namespace vision::execution
