#include "vision/execution/domain/inspection.h"

#include <algorithm>
#include <memory>

namespace vision::execution {

const char* toString(InspectionPurpose p) {
    switch (p) {
        case InspectionPurpose::Production: return "production";
        case InspectionPurpose::Debug: return "debug";
        case InspectionPurpose::Check: return "check";
    }
    return "unknown";
}

const char* toString(NodeStatus s) {
    switch (s) {
        case NodeStatus::Succeeded: return "succeeded";
        case NodeStatus::Failed: return "failed";
        case NodeStatus::Skipped: return "skipped";
    }
    return "unknown";
}

const char* toString(VerdictKind v) {
    switch (v) {
        case VerdictKind::OK: return "OK";
        case VerdictKind::NG: return "NG";
        case VerdictKind::ERROR: return "ERROR";
    }
    return "unknown";
}

Inspection Inspection::start(std::string id, InspectionPurpose purpose, std::string recipeVersionId,
                             std::vector<std::string> plannedOrder) {
    Inspection i;
    i.id_ = std::move(id);
    i.purpose_ = purpose;
    i.recipeVersionId_ = std::move(recipeVersionId);
    i.plannedOrder_ = std::move(plannedOrder);
    return i;
}

Status Inspection::recordNode(NodeExecution execution) {
    // I3：完成后不可修改
    if (completed_) return Status::failure("execution.completed", "检测已完成，不能再记录节点");
    // I1：按计划顺序记录
    const size_t next = executions_.size();
    if (next >= plannedOrder_.size() || plannedOrder_[next] != execution.nodeId) {
        const std::string expected = next < plannedOrder_.size() ? plannedOrder_[next] : "（无）";
        return Status::failure("execution.out_of_order",
                               "节点执行顺序错误：期望 " + expected + "，实际 " + execution.nodeId);
    }
    executions_.push_back(std::move(execution));
    return Status::success();
}

Status Inspection::complete(Verdict verdict) {
    // I3：只能完成一次
    if (completed_) return Status::failure("execution.completed", "检测已经完成");
    if (executions_.size() != plannedOrder_.size()) {
        return Status::failure("execution.incomplete", "还有节点没有执行记录，不能完成检测");
    }
    // I2：有节点失败时，判定必须为 ERROR
    const bool anyFailed = std::any_of(executions_.begin(), executions_.end(),
                                       [](const NodeExecution& e) { return e.status != NodeStatus::Succeeded; });
    if (anyFailed && verdict.kind != VerdictKind::ERROR) {
        return Status::failure("execution.verdict_inconsistent", "有节点执行失败，判定必须为 ERROR");
    }
    // I4：判定为 NG 时，至少有一条不合格的判定依据
    if (verdict.kind == VerdictKind::NG &&
        std::none_of(verdict.reasons.begin(), verdict.reasons.end(), [](const VerdictReason& r) { return !r.pass; })) {
        return Status::failure("execution.verdict_inconsistent", "判定为 NG，但没有不合格的判定依据");
    }

    verdict_ = std::move(verdict);
    completed_ = true;
    if (purpose_ != InspectionPurpose::Debug) {
        record(std::make_shared<InspectionCompleted>(id_, recipeVersionId_, purpose_, verdict_));
    }
    return Status::success();
}

}  // namespace vision::execution
