#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/assistant/domain/workflow.h"
#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::assistant {

using SessionId = std::string;

// ---------- 检测任务书 ----------

// 数据外发策略：可以把什么交给顾问（D7-4）。顾问可能是外部的大模型服务，工厂的产品图像常常是保密的。
enum class DataSharing { NoImages, Thumbnails, Originals };
const char* toString(DataSharing d);

enum class WorkMode { TuneWithinWorkflow, Freeform };

// 值对象：预算
struct Budget {
    int maxRounds = 20;
    double maxSeconds = 300;
    // 加固阶段最多用几轮（达标之后为了扩大余量而继续的轮次，也计入 maxRounds）。0 表示达标就结束。
    int hardeningRounds = 8;
};

// 值对象：检测任务书。用户交给助手的输入。
struct InspectionBrief {
    std::string goal;                       // 要检测什么（文字描述）
    std::string goldenSetId;                // 用哪个基准图集
    std::vector<std::string> blindCaseIds;  // 盲测集；基准图集里其余的用例是调参集
    WorkMode mode = WorkMode::TuneWithinWorkflow;
    DataSharing dataSharing = DataSharing::NoImages;
};

// ---------- 评估 ----------

// 值对象：一次评估。数字来自检测引擎的实际运行，由上线管理的评估规则算出（A4）。
struct Evaluation {
    std::vector<std::string> caseIds;  // 这次评估覆盖了哪些用例
    int truthOk = 0;
    int truthNg = 0;
    int misses = 0;       // 漏检
    int falseAlarms = 0;  // 误报
    int errors = 0;
    double meanMs = 0;
    double maxMs = 0;
    bool meetsCriteria = false;
    std::vector<std::string> wrongCaseIds;

    int total() const { return static_cast<int>(caseIds.size()); }
};

// 一个用例上观察到的情况。只有调参集的用例会交给顾问（A3）。
struct CaseObservation {
    std::string caseId;
    bool truthNg = false;
    std::string verdict;                         // OK / NG / ERROR
    std::vector<std::string> reasons;            // 判定依据
    std::map<std::string, std::string> outputs;  // “节点.端口” → 文字摘要，例如 "blobs.count" → "number 3"
    std::string imageRef;                        // 图像。数据外发策略不允许时为空（A9）
    bool thumbnailOnly = false;                  // 只允许外发缩略图
};

// 稳健性检查的一项结果：对调参集的图像做一种已知的扰动之后重新评估
struct RobustnessResult {
    std::string kind;  // brightness / noise
    double level = 0;
    int total = 0, misses = 0, falseAlarms = 0, errors = 0;
    bool meetsCriteria = false;

    int wrong() const { return misses + falseAlarms + errors; }
};

// 两次评估相比
enum class Comparison { Better, Equal, Worse };

// a 是否比 b 更稳。先比“通不过的扰动有几项”——它决定了方案能在多大的范围里用；
// 一样多再比判错的总数，再一样比漏检。
// 这里故意不用“漏检优先”：那样会拿很多项扰动下的误报去换一项扰动下的漏检，可用的范围反而变窄。
bool moreRobust(const std::vector<RobustnessResult>& a, const std::vector<RobustnessResult>& b);

// 稳健性检查中某个用例在某种扰动下的表现。只有调参集的用例会交给顾问（A3）。
struct ProbeObservation {
    std::string kind;
    double level = 0;
    CaseObservation observation;
};

// ---------- 轮次 ----------

enum class RoundOutcome {
    Pending,          // 还在进行
    Applied,          // 提议已应用到候选方案
    RejectedByRules,  // 违反助手的规则（A10）
    RejectedByEditor, // 方案编排拒绝了编辑命令（A2）
    AdvisorFinished,  // 顾问建议结束
    Abandoned,        // 这一轮进行到一半，会话就结束了（取消、顾问出错）
};
enum class RoundDecision { None, Kept, RolledBack };

// 实体：一轮。每一轮都留下完整的记录（A5）。
struct Round {
    int index = 0;
    Proposal proposal;
    RoundOutcome outcome = RoundOutcome::Pending;
    std::string rejectReason;
    std::optional<Evaluation> evaluation;  // 被拒绝的轮次没有
    std::vector<RobustnessResult> robustness;  // 只有调参集达标的轮次才做稳健性检查
    bool hardening = false;                    // 这一轮属于加固阶段
    RoundDecision decision = RoundDecision::None;
    double seconds = 0;
};

// ---------- 交给顾问的内容 ----------

struct TunableState {
    Tunable tunable;
    double current = 0;
    double best = 0;
};

struct RoundSummary {
    int index = 0;
    std::vector<ProposedChange> changes;
    std::string outcome;  // applied / rejected / finished
    std::string rejectReason;
    std::string decision; // kept / rolled-back / none
    int misses = 0, falseAlarms = 0, errors = 0;
    int robustnessWrong = -1;   // 稳健性检查里判错的总数；没有做则为 -1
    int robustnessFailed = -1;  // 稳健性检查里通不过的有几项；没有做则为 -1
};

// 顾问能看到的全部内容。由聚合自己生成，保证不含盲测集（A3）并符合数据外发策略（A9）。
struct AdvisorView {
    std::string goal;
    std::string workflowName;
    std::string workflowDescription;
    std::vector<WorkflowNode> nodes;
    std::vector<TunableState> tunables;
    std::optional<Evaluation> best;            // 目前最好的评估（调参集）
    std::vector<RoundSummary> history;
    std::vector<CaseObservation> observations; // 当前候选方案在调参集上的表现
    int roundsLeft = 0;
    DataSharing dataSharing = DataSharing::NoImages;
    // 加固阶段：调参集已经达标，现在的目标是扩大余量——让稳健性检查里判错的更少，同时调参集不能变差
    bool hardening = false;
    std::vector<RobustnessResult> robustness;  // 目前最好的候选方案的稳健性检查结果（达标之后才有）
    // 稳健性检查里判错的用例在扰动之后的表现（每种扰动给几个）：顾问靠它看出“扰动之后数字变成了多少”
    std::vector<ProbeObservation> probeObservations;
};

// ---------- 报告 ----------

enum class EndReason { None, CriteriaMet, BudgetExhausted, AdvisorFinished, Cancelled, Failed };
const char* toString(EndReason r);

// 值对象：助手报告。给人做决定用，所以要如实（docs/ddd/07-recipe-assistant.md §8.3）。
struct AssistReport {
    EndReason endReason = EndReason::None;
    Evaluation baseline;               // 工作流初始参数在调参集上的评估
    Evaluation tuning;                 // 最好的候选方案在调参集上的评估
    std::optional<Evaluation> blind;   // 在盲测集上的评估
    bool overfitSuspected = false;     // 调参集达标而盲测集明显变差
    std::vector<RobustnessResult> robustness;
    std::map<std::string, double> initialValues;
    std::map<std::string, double> finalValues;
    // 加固：刚达标时的参数和稳健性检查结果，用来和最终的对比。没有达标或没有加固则为空
    std::map<std::string, double> valuesWhenFirstMet;
    std::vector<RobustnessResult> robustnessWhenFirstMet;
    int hardeningRounds = 0;
    int rounds = 0, kept = 0, rolledBack = 0, rejected = 0;
    double seconds = 0;
    std::string advisor;
    std::vector<std::string> notes;    // 提示和已知的局限
};

// ---------- 聚合 ----------

enum class SessionStatus { InProgress, Ended, Adopted, Abandoned };
const char* toString(SessionStatus s);

// 聚合的全部状态。单独成一个结构体，方便保存和恢复。
struct SessionState {
    SessionId id;
    InspectionBrief brief;
    Budget budget;
    std::string advisor;                    // 用的是哪个顾问实现（只作记录）
    std::string candidateRecipeId;          // 候选方案：助手修改的是它，不是用户自己的草稿（A1）
    std::vector<std::string> tuningCaseIds;
    std::map<std::string, double> initialValues, currentValues, bestValues;  // “节点.参数” → 值
    std::optional<Evaluation> baseline, best;
    std::optional<std::vector<RobustnessResult>> bestRobustness;  // 最好的候选方案达标时才有
    std::map<std::string, double> valuesWhenFirstMet;
    std::vector<RobustnessResult> robustnessWhenFirstMet;
    int hardeningRounds = 0;  // 加固阶段已经开始了几轮
    std::vector<Round> rounds;
    SessionStatus status = SessionStatus::InProgress;
    EndReason endReason = EndReason::None;
    std::optional<Evaluation> blind;
    std::vector<RobustnessResult> robustness;
    bool robustnessRecorded = false;
    std::optional<AssistReport> report;
};

// 聚合根：助手会话。助手针对一份任务书的一次完整工作过程（docs/ddd/07-recipe-assistant.md §8.1）。
//
// 不变量：
//   A1  只能修改候选方案；不能发布；不能直接修改用户的草稿
//       （本聚合和它的端口里根本没有“发布”这个操作）
//   A2  提议只能通过方案编排的编辑命令应用；编辑命令被拒绝时如实记录
//   A3  顾问只能看到调参集：advisorView() 不会包含盲测集的任何内容
//   A4  评估必须来自实际运行：评估必须正好覆盖调参集（或盲测集）的全部用例
//   A5  每一轮都留下完整记录；上一轮没有结束，不能开始下一轮
//   A6  超出预算立即结束
//   A7  盲测集在一次会话中只评估一次；评估之后不能再迭代
//   A8  只有人可以采纳：adopt() 只在会话结束并且有了报告之后才被接受
//   A9  交给顾问的内容符合数据外发策略
//   A10 工作流内调参时，提议只能修改声明的可调项，且在声明的范围内
//   A11 加固不能以调参集为代价：达标之后，只有“调参集的评估不变差，并且稳健性检查里判错的更少”的修改才保留
class AssistSession : public EventRecorder {
public:
    // allCaseIds：基准图集的全部用例。盲测集必须是它的子集，并且调参集不能为空。
    static Result<AssistSession> start(SessionId id, InspectionBrief brief, Budget budget, Workflow workflow,
                                       std::string advisor, const std::vector<std::string>& allCaseIds,
                                       std::string candidateRecipeId);
    static AssistSession restore(SessionState state, Workflow workflow);

    // 这次评估之后要不要接着做稳健性检查：调参集达标，并且工作流声明了稳健性检查、加固预算不为 0。
    bool needsRobustness(const Evaluation& evaluation) const;

    // 工作流初始参数的评估。必须在第一轮之前记录。needsRobustness() 为真时必须同时给出稳健性检查的结果。
    // 如果已经达标并且没有可加固的余地，会话直接结束。
    Status recordBaseline(Evaluation evaluation, std::optional<std::vector<RobustnessResult>> robustness = std::nullopt);

    // 开始新的一轮，返回轮次编号。预算用尽时会话结束并返回失败（A6）。
    Result<int> beginRound(double elapsedSeconds);

    // 提交顾问的提议。
    //   合法：返回要对候选方案执行的编辑；
    //   不合法（A10）：这一轮记为“被规则拒绝”并结束，返回失败，错误信息就是要告诉顾问的原因；
    //   顾问建议结束：会话结束，返回空的编辑列表。
    Result<std::vector<TunableEdit>> submitProposal(Proposal proposal);

    // 方案编排拒绝了编辑命令（A2）。这一轮结束。返回把候选方案恢复原状要执行的编辑。
    std::vector<TunableEdit> editorRejected(const std::string& reason, double roundSeconds);

    // 提议已应用，并且候选方案在调参集上实际运行过了。
    //   tuning：这次评估和目前最好的相比如何，由上线管理的评估规则判断。
    //   robustness：needsRobustness(evaluation) 为真时必须给出（实际运行得到的），否则必须为空。
    // 保留的条件：调参集的评估更好；或者一样好、并且更稳（moreRobust，A11）。
    // 否则回退，返回把候选方案退回到最好状态要执行的编辑。
    // 保留之后：达标并且稳健性检查全部通过（或者没有加固这回事），会话结束。
    Result<std::vector<TunableEdit>> recordEvaluation(Evaluation evaluation, Comparison tuning,
                                                      std::optional<std::vector<RobustnessResult>> robustness,
                                                      double roundSeconds);

    // 因为取消、出错、预算用尽而结束
    Status end(EndReason reason);

    // 会话结束之后：盲测集评估（只能一次，A7）、稳健性检查、生成报告
    Status recordBlindEvaluation(Evaluation evaluation);
    Status recordRobustness(std::vector<RobustnessResult> results);
    Status generateReport(double totalSeconds);

    // 只有人可以采纳（A8）
    Status adopt();
    Status abandon();

    // 交给顾问的内容（A3、A9）。observations 是当前候选方案在各个用例上的表现；
    // 即使调用的人不小心把盲测集的用例也放了进来，这里也会把它们去掉。
    AdvisorView advisorView(const std::vector<CaseObservation>& observations,
                            const std::vector<ProbeObservation>& probeObservations = {}) const;

    const SessionState& state() const { return s_; }
    const Workflow& workflow() const { return workflow_; }
    const SessionId& id() const { return s_.id; }
    SessionStatus status() const { return s_.status; }
    bool blindEvaluated() const { return s_.blind.has_value(); }
    // 是否处于加固阶段：还在进行，并且目前最好的候选方案已经在调参集上达标
    bool hardening() const;
    // 当前进行中的一轮；没有则为空
    const Round* openRound() const;

private:
    AssistSession(SessionState state, Workflow workflow) : s_(std::move(state)), workflow_(std::move(workflow)) {}
    bool isBlind(const std::string& caseId) const;
    std::optional<CaseObservation> forAdvisor(const CaseObservation& o) const;
    Status requireCoverage(const Evaluation& e, const std::vector<std::string>& expected, const char* what) const;
    Status requireRobustness(const Evaluation& e, const std::optional<std::vector<RobustnessResult>>& robustness) const;
    bool hardeningEnabled() const;
    void afterNewBest();
    std::vector<TunableEdit> editsToReach(const std::map<std::string, double>& target) const;
    void finish(EndReason reason);
    Round* open();

    SessionState s_;
    Workflow workflow_;
    std::map<std::string, double> pendingValues_;  // 提议通过了规则检查、等待应用的取值
};

// ---------- 领域事件（docs/ddd/01-event-storming.md H 组）----------

struct AssistSessionStarted final : DomainEvent {
    std::string sessionId, workflowId;
    AssistSessionStarted(std::string id, std::string w) : sessionId(std::move(id)), workflowId(std::move(w)) {}
    const char* name() const override { return "AssistSessionStarted"; }
};

struct ProposalRejected final : DomainEvent {
    std::string sessionId, reason;
    int round;
    ProposalRejected(std::string id, int r, std::string why) : sessionId(std::move(id)), reason(std::move(why)), round(r) {}
    const char* name() const override { return "ProposalRejected"; }
};

struct ExperimentCompleted final : DomainEvent {
    std::string sessionId;
    int round;
    bool kept;
    ExperimentCompleted(std::string id, int r, bool k) : sessionId(std::move(id)), round(r), kept(k) {}
    const char* name() const override { return "ExperimentCompleted"; }
};

struct AssistSessionEnded final : DomainEvent {
    std::string sessionId;
    EndReason reason;
    AssistSessionEnded(std::string id, EndReason r) : sessionId(std::move(id)), reason(r) {}
    const char* name() const override { return "AssistSessionEnded"; }
};

struct BlindEvaluationCompleted final : DomainEvent {
    std::string sessionId;
    explicit BlindEvaluationCompleted(std::string id) : sessionId(std::move(id)) {}
    const char* name() const override { return "BlindEvaluationCompleted"; }
};

struct AssistReportGenerated final : DomainEvent {
    std::string sessionId;
    explicit AssistReportGenerated(std::string id) : sessionId(std::move(id)) {}
    const char* name() const override { return "AssistReportGenerated"; }
};

struct CandidateAdopted final : DomainEvent {
    std::string sessionId, recipeId;
    CandidateAdopted(std::string id, std::string r) : sessionId(std::move(id)), recipeId(std::move(r)) {}
    const char* name() const override { return "CandidateAdopted"; }
};

struct CandidateAbandoned final : DomainEvent {
    std::string sessionId;
    explicit CandidateAbandoned(std::string id) : sessionId(std::move(id)) {}
    const char* name() const override { return "CandidateAbandoned"; }
};

// ---------- 盲测集的划分 ----------

// 从基准图集里选出盲测集：合格、不合格两类各自按比例抽取，保证两类在盲测集里都有（只要这一类不少于 2 个）。
// 同样的输入和种子，在任何平台上结果都相同。
std::vector<std::string> chooseBlindCases(const std::vector<std::pair<std::string, bool>>& casesWithTruthNg, double fraction,
                                          unsigned seed);

}  // namespace vision::assistant
