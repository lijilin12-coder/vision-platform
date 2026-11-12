#pragma once

#include <map>
#include <string>
#include <vector>

#include "vision/common/result.h"
#include "vision/execution/domain/inspection.h"
#include "vision/execution/domain/plan.h"
#include "vision/kernel/image.h"

namespace vision::execution {

// 领域服务：判定。汇总所有判定依据得出判定（D4 / Q-6：结果项各自给出判定依据，由 Judge 汇总）。
//   - 有节点失败或被跳过 → ERROR
//   - 有任何不合格的判定依据 → NG
//   - 否则 → OK
class Judge {
public:
    Verdict judge(const std::vector<NodeExecution>& executions, const std::vector<VerdictReason>& reasons) const;
};

// 执行结果：每个节点的输出（调试时用来查看中间结果）。
using NodeOutputs = std::map<std::string, PortValues>;

// 领域服务：按执行计划依次运行节点，把每个节点的结果记录进 Inspection，最后判定并完成检测。
class Executor {
public:
    Result<NodeOutputs> run(const ExecutionPlan& plan, const Image& frame, Inspection& inspection) const;

private:
    Judge judge_;
};

}  // namespace vision::execution
