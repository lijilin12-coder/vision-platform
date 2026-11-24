#pragma once

// BC-3 检测执行的公开接口。

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vision/kernel/model.h"
#include "vision/authoring/api.h"  // 只用到发布语言 RecipeVersionDocument
#include "vision/catalog/api.h"
#include "vision/common/domain_event.h"
#include "vision/common/result.h"
#include "vision/kernel/image.h"
#include "vision/kernel/match_template.h"
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
    // 端口 → 统计信息（灰度分布、各连通域的面积……）。只有调试运行才计算：它要把图像扫一遍，产线上不值得花这个时间。
    std::map<std::string, std::string> statistics;
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

// 端口：按内容哈希取出匹配模板的图像（docs/ddd/09-match-template-and-locating.md §8）。
// 实现必须校验内容的哈希：文件被改过或者损坏时返回失败，而不是悄悄交出一幅错的模板。
class TemplateSource {
public:
    virtual ~TemplateSource() = default;
    virtual Result<Image> load(const authoring::RecipeDocumentTemplate& ref) = 0;
};

// 端口：按模型版本 ID 取出一个已经加载好、可以运行的模型。
// requirePublished：正式检测（产线、上线前的检查）只能用已经发布的模型版本；调试运行可以用未发布的——
// 刚训练出来的模型要先试一试，才知道该不该发布。
class ModelSource {
public:
    virtual ~ModelSource() = default;
    virtual Result<std::shared_ptr<const InferenceModel>> load(const std::string& modelVersionId, bool requirePublished) = 0;
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

    // 用到匹配模板的方案需要它。模板在编译执行计划时取出、解码，之后每次检测没有额外开销。
    void useTemplateSource(TemplateSource& source);
    // 用到模型的方案需要它。模型在编译执行计划时加载，之后每次检测直接运行。
    void useModelSource(ModelSource& source);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 输出值的文字摘要，例如 "image 640x480"、"blobs 3"。
std::string describeValue(const Value& value);

// 输出值的统计信息，给“看不到图像”的人或程序判断参数该怎么调：
//   图像：灰度的最小值、几个分位数、最大值；区域：选中像素的比例；连通域：按面积从大到小列出。
// 没有可说的（数字、文字等）返回空字符串。
std::string describeStatistics(const Value& value);

}  // namespace vision::execution
