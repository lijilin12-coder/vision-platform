#pragma once

// BC-11 方案助手的公开接口。只有方案构建平台使用；产线运行的程序不链接它（ADR-28）。
//
// 助手不直接依赖其他上下文。它需要的能力都写成下面的端口，由组装根接到
// 方案编排、检测执行、上线管理上。这样做有两个好处：
//   1. 助手能做什么，看端口就一清二楚——端口里没有“发布”，助手就不可能发布方案（A1）；
//   2. 测试时可以换成假的实现，不需要 OpenCV。

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"
#include "vision/kernel/image.h"
#include "vision/kernel/signature.h"

namespace vision::assistant {

class RecipeAdvisor;            // 领域层：方案顾问
class WorkflowRepository;       // 领域层
class AssistSessionRepository;  // 领域层

// ===== 端口 =====

// 候选方案的编辑（接到方案编排的编辑命令上，A2）。注意：没有发布。
class CandidateEditor {
public:
    virtual ~CandidateEditor() = default;
    virtual Result<std::string> createRecipe(const std::string& name) = 0;
    virtual Result<std::string> addNode(const std::string& recipeId, const std::string& operatorId) = 0;
    virtual Status connect(const std::string& recipeId, const std::string& from, const std::string& to) = 0;
    virtual Status setParam(const std::string& recipeId, const std::string& nodeId, const std::string& param,
                            const ParamValue& value) = 0;
    // 从 source 上框出一块作为匹配模板，返回模板 ID（工作流有模板槽时才会用到）
    virtual Result<std::string> createTemplate(const std::string& recipeId, const std::string& name, const Image& source,
                                               int x, int y, int width, int height) = 0;
};

// 对图像做的已知扰动，用于稳健性检查
struct Perturbation {
    std::string kind;  // brightness（乘以 level）/ noise（标准差为 level 的高斯噪声）
    double level = 0;
};

struct TrialResult {
    std::string verdict;  // OK / NG / ERROR
    double durationMs = 0;
    std::vector<std::string> reasons;
    std::map<std::string, std::string> outputs;  // “节点ID.端口” → 文字摘要
};

// 试运行（接到检测引擎上）：用候选方案的当前草稿检测一幅图像。
class TrialRunner {
public:
    virtual ~TrialRunner() = default;
    virtual Result<TrialResult> run(const std::string& recipeId, const std::string& imageRef,
                                    const std::optional<Perturbation>& perturbation) = 0;
    // 用候选方案的当前草稿运行一幅图像，取出某个节点某个输出端口上的图像。
    // 用来从“经过了前面的预处理的图像”上框模板：模板必须取自定位节点实际看到的图像。
    // 这时候候选方案里用到模板的节点还没有模板，会运行失败；只要它上游的节点正常运行就够了。
    virtual Result<Image> outputImage(const std::string& recipeId, const std::string& imageRef, const std::string& nodeId,
                                      const std::string& port) = 0;
};

struct CaseInfo {
    std::string id;
    std::string imageRef;
    bool truthNg = false;
};

struct CaseOutcome {
    std::string caseId;
    std::string verdict;
    double durationMs = 0;
};

struct EvaluationData {
    int truthOk = 0, truthNg = 0, misses = 0, falseAlarms = 0, errors = 0;
    double meanMs = 0, maxMs = 0;
    bool meetsCriteria = false;
    std::vector<std::string> wrongCaseIds;
};

// 基准图集和评估规则（接到上线管理上）。
// 怎么算漏检、误报、达标，以及两次评估哪个更好，都由上线管理说了算，助手不另立一套。
class GoldenCases {
public:
    virtual ~GoldenCases() = default;
    virtual Result<std::vector<CaseInfo>> cases(const std::string& goldenSetId) = 0;
    virtual Result<EvaluationData> evaluate(const std::string& goldenSetId, const std::vector<CaseOutcome>& outcomes) = 0;
    virtual bool betterThan(const EvaluationData& a, const EvaluationData& b) = 0;
};

// ===== 应用服务 =====

// 任务书里给工作流的一个模板槽提供的模板：用哪个用例的图像，框哪一块
struct TemplateRequest {
    std::string slot;
    std::string caseId;
    int x = 0, y = 0, width = 0, height = 0;
};

// 检测任务书（用户提交的内容）
struct AssistRequest {
    std::string goal;
    std::string goldenSetId;
    std::string workflowId;
    // 盲测集：直接指定用例；不指定时按 blindFraction 从基准图集里分层抽取
    std::vector<std::string> blindCaseIds;
    double blindFraction = 0.3;
    unsigned seed = 1;
    std::string dataSharing = "no-images";  // no-images / thumbnails / originals
    int maxRounds = 20;
    double maxSeconds = 300;
    int hardeningRounds = 8;  // 达标之后最多再用几轮扩大余量；0 表示达标就结束
    std::vector<TemplateRequest> templates;  // 工作流有模板槽时，每个槽一个；用例必须属于调参集
};

struct SessionView {
    std::string sessionId;
    std::string candidateRecipeId;
    std::string status;     // in-progress / ended / adopted / abandoned
    std::string endReason;
    std::string advisor;
    int rounds = 0, kept = 0, rolledBack = 0, rejected = 0;
    int hardeningRounds = 0;  // 其中加固阶段的轮数
    std::vector<double> roundSeconds;
    double seconds = 0;
    bool tuningMeetsCriteria = false;
    std::optional<bool> blindMeetsCriteria;
    bool overfitSuspected = false;
    std::map<std::string, double> finalValues;
    std::vector<std::string> notes;
    std::string reportMarkdown;  // 完整的助手报告
};

class AssistService {
public:
    AssistService(WorkflowRepository& workflows, AssistSessionRepository& sessions, RecipeAdvisor& advisor,
                  CandidateEditor& editor, TrialRunner& runner, GoldenCases& golden, EventPublisher& events);

    // 完成一次助手会话：搭出候选方案 → 迭代调参 → 盲测 → 稳健性检查 → 报告。
    // 结束时候选方案停在“目前最好”的状态，等人来决定采纳还是放弃。
    Result<SessionView> run(const AssistRequest& request);

    // 只有人可以采纳（A8）：这两个命令由界面在用户点了按钮之后调用。
    // 采纳之后候选方案就是用户的一份普通草稿，是否发布仍然由用户决定。
    Status adopt(const std::string& sessionId);
    Status abandon(const std::string& sessionId);

    Result<SessionView> getSession(const std::string& sessionId) const;
    std::vector<std::string> listSessions() const;

private:
    WorkflowRepository& workflows_;
    AssistSessionRepository& sessions_;
    RecipeAdvisor& advisor_;
    CandidateEditor& editor_;
    TrialRunner& runner_;
    GoldenCases& golden_;
    EventPublisher& events_;
};

}  // namespace vision::assistant
