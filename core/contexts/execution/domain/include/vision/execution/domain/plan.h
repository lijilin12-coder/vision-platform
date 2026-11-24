#pragma once

// 检测执行上下文自己的“方案”模型：执行计划（docs/ddd/04-context-map.md §4）。
// 它和方案编排里的 Recipe 是同一个词在不同上下文中的不同模型：
// 这里不关心画布、草稿、撤销，只关心执行顺序和数据从哪来到哪去。

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/result.h"
#include "vision/kernel/geometry.h"
#include "vision/kernel/match_template.h"
#include "vision/kernel/model.h"
#include "vision/kernel/signature.h"
#include "vision/kernel/value.h"

namespace vision::execution {

// ---- 编译执行计划的输入 ----

struct PlanNode {
    std::string id;
    OperatorSignature signature;
    ParamMap params;
    std::optional<Roi> roi;
    // 模板参数名 → 已经解码好的匹配模板。编译执行计划之前就准备好，之后每次检测不再读取、解码
    std::map<std::string, MatchTemplateData> templates;
    // 模型参数名 → 已经加载好的模型。同样在编译执行计划之前准备好
    std::map<std::string, std::shared_ptr<const InferenceModel>> models;
};

struct PlanEdge {
    std::string fromNode, fromPort, toNode, toPort;
};

struct PlanSpec {
    std::string recipeVersionId;
    std::vector<PlanNode> nodes;
    std::vector<PlanEdge> edges;
};

// ---- 端口：节点运行器（由应用层把算子目录里的算子适配进来）----

using PortValues = std::map<std::string, Value>;

class NodeRunner {
public:
    virtual ~NodeRunner() = default;
    virtual Result<PortValues> run(const PortValues& inputs, const ParamMap& params,
                                   const std::optional<Roi>& roi) = 0;
};

class NodeRunnerFactory {
public:
    virtual ~NodeRunnerFactory() = default;
    // 运行器在创建时就拿到整个节点的定义（包括它的匹配模板），之后每次 run 只需要这一次检测的数据
    virtual Result<std::shared_ptr<NodeRunner>> create(const PlanNode& node) = 0;
};

// ---- 值对象：执行计划 ----

struct InputBinding {
    size_t sourceStep;       // 上游步骤的下标
    std::string sourcePort;  // 上游的输出端口
};

struct PlanStep {
    PlanNode node;
    std::map<std::string, InputBinding> inputs;  // 输入端口 → 数据来源
    std::shared_ptr<NodeRunner> runner;           // 图像入口节点为空
    bool isImageInput = false;
};

class ExecutionPlan {
public:
    ExecutionPlan(std::string recipeVersionId, std::vector<PlanStep> steps)
        : recipeVersionId_(std::move(recipeVersionId)), steps_(std::move(steps)) {}

    const std::string& recipeVersionId() const { return recipeVersionId_; }
    const std::vector<PlanStep>& steps() const { return steps_; }
    std::vector<std::string> nodeOrder() const;

private:
    std::string recipeVersionId_;
    std::vector<PlanStep> steps_;
};

// ---- 领域服务：把方案编译成执行计划 ----
// 拓扑排序（Kahn 算法）、绑定输入、实例化运行器。
// 方案编排已经保证了无环和类型兼容，这里仍然防御性地再检查一次：
// 执行计划可能来自导入的方案包，不能假设上游一定正确。
class ExecutionPlanner {
public:
    explicit ExecutionPlanner(NodeRunnerFactory& factory) : factory_(factory) {}
    Result<ExecutionPlan> plan(const PlanSpec& spec) const;

private:
    NodeRunnerFactory& factory_;
};

}  // namespace vision::execution
