#pragma once

// BC-3 检测执行的公开接口。

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vision/authoring/api.h"  // 只用到发布语言 RecipeVersionDocument
#include "vision/catalog/api.h"
#include "vision/common/domain_event.h"
#include "vision/common/result.h"
#include "vision/kernel/image.h"
#include "vision/kernel/value.h"

namespace vision::execution {

enum class RunPurpose { Production, Debug, Check };

struct NodeReport {
    std::string nodeId;
    std::string operatorId;
    std::string status;  // succeeded / failed / skipped
    double durationMs = 0;
    std::string message;
    std::map<std::string, std::string> outputs;  // 端口 → 文字摘要
};

struct ReasonReport {
    std::string nodeId;
    bool pass = true;
    std::string text;
};

struct InspectionReport {
    std::string inspectionId;
    std::string recipeVersionId;
    std::string purpose;
    std::string verdict;  // OK / NG / ERROR
    std::vector<ReasonReport> reasons;
    std::vector<NodeReport> nodes;
    double totalMs = 0;
};

// 调试运行：额外返回每个节点的完整输出，用于查看中间结果（AUT-04）。
struct DebugReport {
    InspectionReport report;
    std::map<std::string, std::map<std::string, Value>> outputs;  // 节点 → 端口 → 值
};

// 应用服务：检测引擎。被方案编排（调试）、上线管理（检查）、工位运行（产线）复用（D4-2）。
class ExecutionService {
public:
    ExecutionService(const catalog::OperatorCatalog& catalog, EventPublisher& events);
    ~ExecutionService();
    ExecutionService(const ExecutionService&) = delete;
    ExecutionService& operator=(const ExecutionService&) = delete;

    Result<InspectionReport> inspect(const authoring::RecipeVersionDocument& recipe, const Image& frame,
                                     RunPurpose purpose = RunPurpose::Production);
    Result<DebugReport> debugRun(const authoring::RecipeVersionDocument& recipe, const Image& frame);

    // 预热：提前把方案版本编译成执行计划并缓存起来，之后第一次检测就不用再花这个时间。
    // 工位在后台加载上线包时调用（规则 P8、事件 G2）。可以和 inspect 在不同的线程里同时调用。
    Status prepare(const authoring::RecipeVersionDocument& recipe);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 输出值的文字摘要，例如 "image 640x480"、"blobs 3"。
std::string describeValue(const Value& value);

}  // namespace vision::execution
