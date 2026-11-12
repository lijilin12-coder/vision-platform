#pragma once

#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::execution {

enum class InspectionPurpose { Production, Debug, Check };
enum class NodeStatus { Succeeded, Failed, Skipped };
// 注意：不能叫 ERROR。Windows 的系统头文件把 ERROR 定义成了宏，一旦被包含就会编译失败。
enum class VerdictKind { Ok, Ng, Error };

const char* toString(InspectionPurpose p);
const char* toString(NodeStatus s);
const char* toString(VerdictKind v);

// 实体：节点执行记录
struct NodeExecution {
    std::string nodeId;
    NodeStatus status = NodeStatus::Succeeded;
    double durationMs = 0;
    std::string message;  // 失败或跳过的原因
};

// 值对象：判定依据
struct VerdictReason {
    std::string nodeId;
    bool pass = true;
    std::string text;
};

// 值对象：判定
struct Verdict {
    VerdictKind kind = VerdictKind::Ok;
    std::vector<VerdictReason> reasons;
};

// 聚合根：一次检测（docs/ddd/05-tactical-design.md §3.2）
//
// 不变量：
//   I1 节点执行按计划顺序记录
//   I2 有节点执行失败时，判定必须为 ERROR
//   I3 判定只在完成时设置一次；完成后整个聚合不可修改
//   I4 判定为 NG 时，至少有一条不合格的判定依据
//   另：完成时，计划中的每个节点都必须有执行记录
class Inspection : public EventRecorder {
public:
    static Inspection start(std::string id, InspectionPurpose purpose, std::string recipeVersionId,
                            std::vector<std::string> plannedOrder);

    Status recordNode(NodeExecution execution);
    Status complete(Verdict verdict);

    const std::string& id() const { return id_; }
    InspectionPurpose purpose() const { return purpose_; }
    const std::string& recipeVersionId() const { return recipeVersionId_; }
    bool completed() const { return completed_; }
    const std::vector<NodeExecution>& nodeExecutions() const { return executions_; }
    const Verdict& verdict() const { return verdict_; }

private:
    Inspection() = default;

    std::string id_;
    InspectionPurpose purpose_ = InspectionPurpose::Production;
    std::string recipeVersionId_;
    std::vector<std::string> plannedOrder_;
    std::vector<NodeExecution> executions_;
    Verdict verdict_;
    bool completed_ = false;
};

// 领域事件：检测已完成。调试用途的检测不发布它（避免调试数据进入统计和数据集）。
struct InspectionCompleted final : DomainEvent {
    std::string inspectionId;
    std::string recipeVersionId;
    InspectionPurpose purpose;
    Verdict verdict;
    InspectionCompleted(std::string id, std::string rv, InspectionPurpose p, Verdict v)
        : inspectionId(std::move(id)), recipeVersionId(std::move(rv)), purpose(p), verdict(std::move(v)) {}
    const char* name() const override { return "InspectionCompleted"; }
};

}  // namespace vision::execution
