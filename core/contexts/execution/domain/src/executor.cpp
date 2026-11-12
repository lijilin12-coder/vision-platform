#include "vision/execution/domain/executor.h"

#include <algorithm>
#include <chrono>

namespace vision::execution {

Verdict Judge::judge(const std::vector<NodeExecution>& executions, const std::vector<VerdictReason>& reasons) const {
    Verdict v;
    std::vector<VerdictReason> errors;
    for (const auto& e : executions) {
        if (e.status == NodeStatus::Failed) errors.push_back({e.nodeId, false, "节点执行失败：" + e.message});
    }
    if (!errors.empty()) {
        v.kind = VerdictKind::Error;
        v.reasons = std::move(errors);
        return v;
    }
    v.reasons = reasons;
    const bool anyFail = std::any_of(reasons.begin(), reasons.end(), [](const VerdictReason& r) { return !r.pass; });
    v.kind = anyFail ? VerdictKind::Ng : VerdictKind::Ok;
    return v;
}

Result<NodeOutputs> Executor::run(const ExecutionPlan& plan, const Image& frame, Inspection& inspection) const {
    using Clock = std::chrono::steady_clock;
    const auto& steps = plan.steps();
    std::vector<PortValues> outputs(steps.size());
    std::vector<bool> succeeded(steps.size(), false);
    std::vector<VerdictReason> reasons;

    for (size_t i = 0; i < steps.size(); ++i) {
        const PlanStep& step = steps[i];
        NodeExecution exec{step.node.id, NodeStatus::Succeeded, 0, ""};

        // 上游失败或被跳过，则本节点跳过
        PortValues inputs;
        for (const auto& [port, binding] : step.inputs) {
            if (!succeeded[binding.sourceStep]) {
                exec.status = NodeStatus::Skipped;
                exec.message = "上游节点 " + steps[binding.sourceStep].node.id + " 未成功";
                break;
            }
            auto it = outputs[binding.sourceStep].find(binding.sourcePort);
            if (it == outputs[binding.sourceStep].end()) {
                exec.status = NodeStatus::Failed;
                exec.message = "上游节点没有产出端口 " + binding.sourcePort;
                break;
            }
            inputs[port] = it->second;
        }

        if (exec.status == NodeStatus::Succeeded) {
            const auto t0 = Clock::now();
            if (step.isImageInput) {
                if (frame.empty()) {
                    exec.status = NodeStatus::Failed;
                    exec.message = "没有输入图像";
                } else {
                    outputs[i]["image"] = frame;
                }
            } else {
                auto res = step.runner->run(inputs, step.node.params, step.node.roi);
                if (res) {
                    outputs[i] = std::move(res).value();
                } else {
                    exec.status = NodeStatus::Failed;
                    exec.message = res.error().message;
                }
            }
            exec.durationMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        }

        if (exec.status == NodeStatus::Succeeded) {
            succeeded[i] = true;
            // 收集判定依据：Judgement 类型的输出
            for (const auto& out : step.node.signature.outputs) {
                if (out.type != PortType::Judgement) continue;
                auto it = outputs[i].find(out.name);
                if (it != outputs[i].end() && std::holds_alternative<Judgement>(it->second)) {
                    const auto& j = std::get<Judgement>(it->second);
                    reasons.push_back({step.node.id, j.pass, j.reason});
                }
            }
        }
        if (auto st = inspection.recordNode(exec); !st) return st.error();
    }

    if (auto st = inspection.complete(judge_.judge(inspection.nodeExecutions(), reasons)); !st) return st.error();

    NodeOutputs result;
    for (size_t i = 0; i < steps.size(); ++i) result[steps[i].node.id] = std::move(outputs[i]);
    return result;
}

}  // namespace vision::execution
