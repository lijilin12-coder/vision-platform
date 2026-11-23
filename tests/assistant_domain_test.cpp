// BC-11 方案助手领域层的单元测试（POC-11）。只链接领域库：没有 OpenCV，没有网络，没有文件。
// 测试名字里的 A1…A10 对应 docs/ddd/07-recipe-assistant.md §8.2 的不变量。
#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include "vision/assistant/domain/ports.h"
#include "vision/assistant/domain/session.h"

using namespace vision;
using namespace vision::assistant;

namespace {

Workflow brightSpot() {
    auto w = Workflow::create(
        "wf.bright-spot", 1, "亮斑检测", "灰度 → 阈值 → 连通域 → 计数",
        {{"src", "vision.source", {}}, {"bin", "vision.threshold", {}}, {"blobs", "vision.blob", {}}, {"judge", "vision.judge_count", {}}},
        {{"src.image", "bin.image"}, {"bin.image", "blobs.image"}, {"blobs.blobs", "judge.blobs"}},
        {{"bin", "value", 0, 255, 20, "阈值"}, {"blobs", "min_area", 1, 5000, 1, "最小面积"}},
        {{RobustnessProbe::Kind::Brightness, {0.8, 1.2}}});
    EXPECT_TRUE(w.ok());
    return std::move(w).value();
}

const std::vector<std::string> kAll = {"ok1", "ok2", "ok3", "ng1", "ng2", "ng3"};
const std::vector<std::string> kBlind = {"ok3", "ng3"};
const std::vector<std::string> kTuning = {"ok1", "ok2", "ng1", "ng2"};

InspectionBrief brief(DataSharing sharing = DataSharing::NoImages) {
    InspectionBrief b;
    b.goal = "检出亮斑";
    b.goldenSetId = "gs-1";
    b.blindCaseIds = kBlind;
    b.dataSharing = sharing;
    return b;
}

// 不加固的预算：达标就结束。加固阶段有单独的一组测试。
const Budget kNoHardening{20, 300, 0};

AssistSession newSession(Budget budget = kNoHardening, DataSharing sharing = DataSharing::NoImages) {
    auto s = AssistSession::start("s-1", brief(sharing), budget, brightSpot(), "scripted", kAll, "recipe-cand");
    EXPECT_TRUE(s.ok());
    return std::move(s).value();
}

// 调参集上的一次评估：2 合格 2 不合格
Evaluation tuningEval(int misses, int falseAlarms, int errors = 0) {
    Evaluation e;
    e.caseIds = kTuning;
    e.truthOk = 2;
    e.truthNg = 2;
    e.misses = misses;
    e.falseAlarms = falseAlarms;
    e.errors = errors;
    e.meetsCriteria = misses == 0 && falseAlarms == 0 && errors == 0;
    return e;
}

Evaluation blindEval(int misses, int falseAlarms) {
    Evaluation e;
    e.caseIds = kBlind;
    e.truthOk = 1;
    e.truthNg = 1;
    e.misses = misses;
    e.falseAlarms = falseAlarms;
    e.meetsCriteria = misses == 0 && falseAlarms == 0;
    return e;
}

Proposal setParam(const std::string& node, const std::string& param, double value) {
    Proposal p;
    p.changes.push_back({ProposedChange::Kind::SetParam, node, param, value});
    p.rationale = "test";
    return p;
}

// 不涉及稳健性检查时的简写：better 为真表示调参集的评估更好，否则更差
Result<std::vector<TunableEdit>> rec(AssistSession& s, Evaluation e, bool better, double seconds) {
    return s.recordEvaluation(std::move(e), better ? Comparison::Better : Comparison::Worse, std::nullopt, seconds);
}

// 走完一轮“提议被应用并评估”
void playRound(AssistSession& s, Proposal p, const Evaluation& e, bool better) {
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(std::move(p)).ok());
    ASSERT_TRUE(rec(s, e, better, 0.01).ok());
}

std::vector<std::string> eventNames(AssistSession& s) {
    std::vector<std::string> names;
    for (const auto& e : s.pullEvents()) names.emplace_back(e->name());
    return names;
}

}  // namespace

// ---------------- 工作流 ----------------

TEST(Workflow, RejectsInconsistentDefinitions) {
    const std::vector<WorkflowNode> nodes = {{"a", "op.a", {}}, {"b", "op.b", {}}};
    EXPECT_TRUE(Workflow::create("w", 1, "n", "", nodes, {{"a.out", "b.in"}}, {{"a", "p", 0, 10, 5, ""}}, {}).ok());
    EXPECT_FALSE(Workflow::create("", 1, "n", "", nodes, {}, {}, {}).ok());
    EXPECT_FALSE(Workflow::create("w", 1, "n", "", {}, {}, {}, {}).ok());
    EXPECT_FALSE(Workflow::create("w", 1, "n", "", {{"a", "op", {}}, {"a", "op", {}}}, {}, {}, {}).ok());  // 节点重名
    EXPECT_FALSE(Workflow::create("w", 1, "n", "", nodes, {{"a.out", "zzz.in"}}, {}, {}).ok());               // 连线悬空
    EXPECT_FALSE(Workflow::create("w", 1, "n", "", nodes, {}, {{"zzz", "p", 0, 10, 5, ""}}, {}).ok());        // 可调项悬空
    EXPECT_FALSE(Workflow::create("w", 1, "n", "", nodes, {}, {{"a", "p", 0, 10, 11, ""}}, {}).ok());         // 初始值越界
    EXPECT_FALSE(Workflow::create("w", 1, "n", "", nodes, {}, {{"a", "p", 0, 10, 5, ""}, {"a", "p", 0, 10, 5, ""}}, {}).ok());
}

// A10：工作流内调参时，只能改声明的可调项，且在范围内
TEST(Workflow, A10_ValidateProposal) {
    const Workflow w = brightSpot();
    EXPECT_TRUE(w.validate(setParam("bin", "value", 120)).ok());
    EXPECT_TRUE(w.validate(setParam("bin", "value", 0)).ok());    // 边界值是合法的
    EXPECT_TRUE(w.validate(setParam("bin", "value", 255)).ok());

    EXPECT_EQ(w.validate(Proposal{}).error().code, "assistant.empty_proposal");
    EXPECT_EQ(w.validate(setParam("judge", "max", 3)).error().code, "assistant.not_tunable");   // 存在的节点，但参数没有声明可调
    EXPECT_EQ(w.validate(setParam("nope", "value", 3)).error().code, "assistant.not_tunable");
    EXPECT_EQ(w.validate(setParam("bin", "value", 256)).error().code, "assistant.out_of_range");
    EXPECT_EQ(w.validate(setParam("bin", "value", -1)).error().code, "assistant.out_of_range");

    for (const auto kind : {ProposedChange::Kind::AddNode, ProposedChange::Kind::RemoveNode, ProposedChange::Kind::Connect,
                            ProposedChange::Kind::Disconnect, ProposedChange::Kind::SetRoi}) {
        Proposal p;
        p.changes.push_back({kind, "bin", "value", 100});
        EXPECT_EQ(w.validate(p).error().code, "assistant.structure_change");
    }

    Proposal twice = setParam("bin", "value", 100);
    twice.changes.push_back({ProposedChange::Kind::SetParam, "bin", "value", 110});
    EXPECT_EQ(w.validate(twice).error().code, "assistant.duplicate_change");

    // 一份提议里只要有一处不合法，整份都不接受
    Proposal mixed = setParam("bin", "value", 100);
    mixed.changes.push_back({ProposedChange::Kind::SetParam, "blobs", "min_area", 99999});
    EXPECT_FALSE(w.validate(mixed).ok());
}

// ---------------- 会话：开始 ----------------

TEST(AssistSession, StartSplitsTuningAndBlindCases) {
    AssistSession s = newSession();
    EXPECT_EQ(s.state().tuningCaseIds, kTuning);
    EXPECT_EQ(s.state().currentValues.at("bin.value"), 20);
    EXPECT_EQ(s.status(), SessionStatus::InProgress);
    EXPECT_EQ(eventNames(s), std::vector<std::string>{"AssistSessionStarted"});
}

TEST(AssistSession, StartRejectsBadInput) {
    const auto start = [](InspectionBrief b, Budget budget, const std::vector<std::string>& all, std::string recipe = "r") {
        return AssistSession::start("s", std::move(b), budget, brightSpot(), "x", all, std::move(recipe)).ok();
    };
    EXPECT_TRUE(start(brief(), {}, kAll));
    EXPECT_FALSE(start(brief(), {}, kAll, ""));              // 没有候选方案
    EXPECT_FALSE(start(brief(), {0, 10}, kAll));             // 预算为 0
    EXPECT_FALSE(start(brief(), {20, 300, -1}, kAll));       // 加固的轮数是负数
    EXPECT_FALSE(start(brief(), {}, {"ok1", "ng1"}));        // 盲测集里的用例不在基准图集里
    EXPECT_FALSE(start(brief(), {}, kBlind));                // 全部是盲测集，没有东西可以调参
    InspectionBrief freeform = brief();
    freeform.mode = WorkMode::Freeform;
    EXPECT_FALSE(start(freeform, {}, kAll));                 // 自由搭建还不支持
}

// ---------------- 会话：迭代 ----------------

TEST(AssistSession, BaselineIsRequiredBeforeFirstRound) {
    AssistSession s = newSession();
    EXPECT_EQ(s.beginRound(0).error().code, "assistant.no_baseline");
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    EXPECT_FALSE(s.recordBaseline(tuningEval(0, 2)).ok());
    EXPECT_TRUE(s.beginRound(0).ok());
}

TEST(AssistSession, BaselineAlreadyGoodEndsSession) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 0)).ok());
    EXPECT_EQ(s.status(), SessionStatus::Ended);
    EXPECT_EQ(s.state().endReason, EndReason::CriteriaMet);
}

TEST(AssistSession, BetterRoundIsKept_WorseIsRolledBack) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    (void)s.pullEvents();

    // 第 1 轮：变好，保留
    ASSERT_TRUE(s.beginRound(0).ok());
    auto edits = s.submitProposal(setParam("bin", "value", 100));
    ASSERT_TRUE(edits.ok());
    ASSERT_EQ(edits.value().size(), 1u);
    EXPECT_EQ(edits.value()[0].value, 100);
    auto rollback = rec(s, tuningEval(0, 1), true, 0.01);
    ASSERT_TRUE(rollback.ok());
    EXPECT_TRUE(rollback.value().empty());
    EXPECT_EQ(s.state().bestValues.at("bin.value"), 100);
    EXPECT_EQ(s.state().rounds[0].decision, RoundDecision::Kept);

    // 第 2 轮：变差，回退到最好的状态
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 250)).ok());
    rollback = rec(s, tuningEval(2, 0), false, 0.01);
    ASSERT_TRUE(rollback.ok());
    bool restoresThreshold = false;
    for (const auto& e : rollback.value()) restoresThreshold |= (e.nodeKey == "bin" && e.param == "value" && e.value == 100);
    EXPECT_TRUE(restoresThreshold);
    EXPECT_EQ(s.state().currentValues.at("bin.value"), 100);
    EXPECT_EQ(s.state().best->falseAlarms, 1);
    EXPECT_EQ(s.state().rounds[1].decision, RoundDecision::RolledBack);
    EXPECT_EQ(eventNames(s), (std::vector<std::string>{"ExperimentCompleted", "ExperimentCompleted"}));
}

TEST(AssistSession, EndsWhenCriteriaMet) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    playRound(s, setParam("bin", "value", 120), tuningEval(0, 0), true);
    EXPECT_EQ(s.status(), SessionStatus::Ended);
    EXPECT_EQ(s.state().endReason, EndReason::CriteriaMet);
    EXPECT_FALSE(s.beginRound(0).ok());
}

TEST(AssistSession, AdvisorMayFinish) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    ASSERT_TRUE(s.beginRound(0).ok());
    Proposal p;
    p.finish = true;
    auto edits = s.submitProposal(p);
    ASSERT_TRUE(edits.ok());
    EXPECT_TRUE(edits.value().empty());
    EXPECT_EQ(s.state().endReason, EndReason::AdvisorFinished);
    EXPECT_EQ(s.state().rounds[0].outcome, RoundOutcome::AdvisorFinished);
}

// A10 + A5：越界的提议被拒绝，这一轮记录在案，候选方案的取值不变
TEST(AssistSession, A10_IllegalProposalIsRejectedAndRecorded) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    (void)s.pullEvents();

    Proposal structural;
    structural.changes.push_back({ProposedChange::Kind::AddNode, "extra", "", 0});
    const std::vector<Proposal> illegal = {setParam("judge", "max", 5), setParam("bin", "value", 999), structural};
    for (const auto& p : illegal) {
        ASSERT_TRUE(s.beginRound(0).ok());
        EXPECT_FALSE(s.submitProposal(p).ok());
    }
    ASSERT_EQ(s.state().rounds.size(), 3u);
    for (const auto& r : s.state().rounds) {
        EXPECT_EQ(r.outcome, RoundOutcome::RejectedByRules);
        EXPECT_FALSE(r.rejectReason.empty());
        EXPECT_FALSE(r.evaluation.has_value());
        EXPECT_EQ(r.proposal.changes.size(), 1u);  // 被拒绝的提议本身也留了下来
    }
    EXPECT_EQ(s.state().currentValues, s.state().initialValues);
    EXPECT_EQ(eventNames(s), (std::vector<std::string>{"ProposalRejected", "ProposalRejected", "ProposalRejected"}));
    EXPECT_EQ(s.status(), SessionStatus::InProgress);  // 被拒绝不会结束会话，但会消耗一轮预算

    // 顾问在下一轮能看到被拒绝的原因
    const auto view = s.advisorView({});
    ASSERT_EQ(view.history.size(), 3u);
    EXPECT_EQ(view.history[0].outcome, "rejected");
    EXPECT_FALSE(view.history[0].rejectReason.empty());
}

// A2：方案编排拒绝编辑命令时如实记录，并给出恢复原状的编辑
TEST(AssistSession, A2_EditorRejectionIsRecordedAndRestored) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    playRound(s, setParam("bin", "value", 100), tuningEval(0, 1), true);

    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 130)).ok());
    const auto restore = s.editorRejected("参数类型不对", 0.01);
    bool restoresThreshold = false;
    for (const auto& e : restore) restoresThreshold |= (e.nodeKey == "bin" && e.value == 100);
    EXPECT_TRUE(restoresThreshold);
    EXPECT_EQ(s.state().rounds[1].outcome, RoundOutcome::RejectedByEditor);
    EXPECT_EQ(s.state().rounds[1].rejectReason, "参数类型不对");
    EXPECT_EQ(s.state().currentValues.at("bin.value"), 100);
    EXPECT_TRUE(s.beginRound(0).ok());  // 这一轮已经结束，可以开始下一轮
}

// A3：顾问看不到盲测集
TEST(AssistSession, A3_AdvisorNeverSeesBlindCases) {
    AssistSession s = newSession(kNoHardening, DataSharing::Originals);
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());

    std::vector<CaseObservation> observations;
    for (const auto& id : kAll) {  // 故意把盲测集的用例也传进去
        CaseObservation o;
        o.caseId = id;
        o.verdict = "OK";
        o.imageRef = "/img/" + id + ".png";
        observations.push_back(o);
    }
    CaseObservation stranger;
    stranger.caseId = "not-in-set";
    observations.push_back(stranger);

    const auto view = s.advisorView(observations);
    std::set<std::string> seen;
    for (const auto& o : view.observations) seen.insert(o.caseId);
    EXPECT_EQ(seen, std::set<std::string>(kTuning.begin(), kTuning.end()));
    ASSERT_TRUE(view.best.has_value());
    EXPECT_EQ(view.best->caseIds, kTuning);
}

// A3（续）：盲测的结果出来之后，也不会出现在顾问能看到的内容里
TEST(AssistSession, A3_BlindResultIsNotInAdvisorView) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    playRound(s, setParam("bin", "value", 120), tuningEval(0, 0), true);
    ASSERT_TRUE(s.recordBlindEvaluation(blindEval(1, 0)).ok());
    const auto view = s.advisorView({});
    EXPECT_EQ(view.best->caseIds, kTuning);
    EXPECT_EQ(view.best->misses, 0);
}

// A4：评估必须正好覆盖调参集（或盲测集）
TEST(AssistSession, A4_EvaluationMustCoverExactlyTheCases) {
    AssistSession s = newSession();
    Evaluation partial = tuningEval(0, 0);
    partial.caseIds.pop_back();
    partial.truthNg = 1;
    EXPECT_EQ(s.recordBaseline(partial).error().code, "assistant.evaluation_coverage");

    Evaluation withBlind = tuningEval(0, 0);
    withBlind.caseIds.push_back("ok3");  // 混进了盲测集的用例
    withBlind.truthOk = 3;
    EXPECT_EQ(s.recordBaseline(withBlind).error().code, "assistant.evaluation_coverage");

    Evaluation inconsistent = tuningEval(0, 0);
    inconsistent.truthOk = 7;
    EXPECT_EQ(s.recordBaseline(inconsistent).error().code, "assistant.evaluation_inconsistent");

    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 100)).ok());
    EXPECT_FALSE(rec(s, partial, true, 0).ok());
    EXPECT_FALSE(rec(s, blindEval(0, 0), true, 0).ok());  // 拿盲测集的评估冒充
    EXPECT_TRUE(rec(s, tuningEval(0, 1), true, 0).ok());
}

// A5：上一轮没有结束，不能开始下一轮；没有开始一轮，不能提交提议或评估
TEST(AssistSession, A5_OneRoundAtATime) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    EXPECT_FALSE(s.submitProposal(setParam("bin", "value", 100)).ok());
    EXPECT_FALSE(rec(s, tuningEval(0, 1), true, 0).ok());

    ASSERT_TRUE(s.beginRound(0).ok());
    EXPECT_EQ(s.beginRound(0).error().code, "assistant.round_open");
    EXPECT_FALSE(rec(s, tuningEval(0, 1), true, 0).ok());  // 还没有提议
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 100)).ok());
    EXPECT_EQ(s.beginRound(0).error().code, "assistant.round_open");  // 提议已应用但还没有评估
    EXPECT_FALSE(s.submitProposal(setParam("bin", "value", 110)).ok());
    ASSERT_TRUE(rec(s, tuningEval(0, 1), true, 0.5).ok());

    const Round& r = s.state().rounds[0];
    EXPECT_EQ(r.index, 1);
    EXPECT_EQ(r.proposal.rationale, "test");
    EXPECT_TRUE(r.evaluation.has_value());
    EXPECT_DOUBLE_EQ(r.seconds, 0.5);
}

// A6：超出预算立即结束
TEST(AssistSession, A6_BudgetOfRounds) {
    AssistSession s = newSession(Budget{2, 100, 0});
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    playRound(s, setParam("bin", "value", 100), tuningEval(0, 1), true);
    playRound(s, setParam("bin", "value", 110), tuningEval(0, 1), false);
    EXPECT_EQ(s.beginRound(0).error().code, "assistant.budget_exhausted");
    EXPECT_EQ(s.status(), SessionStatus::Ended);
    EXPECT_EQ(s.state().endReason, EndReason::BudgetExhausted);
    EXPECT_EQ(s.state().rounds.size(), 2u);
}

TEST(AssistSession, A6_BudgetOfTime) {
    AssistSession s = newSession(Budget{20, 10, 0});
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    EXPECT_TRUE(s.beginRound(9.9).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 100)).ok());
    ASSERT_TRUE(rec(s, tuningEval(0, 1), true, 0).ok());
    EXPECT_FALSE(s.beginRound(10.0).ok());
    EXPECT_EQ(s.state().endReason, EndReason::BudgetExhausted);
}

// A7：盲测集只评估一次，而且之后不能再迭代
TEST(AssistSession, A7_BlindSetIsEvaluatedOnceAndOnlyAfterEnd) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    EXPECT_EQ(s.recordBlindEvaluation(blindEval(0, 0)).error().code, "assistant.not_ended");  // 还在迭代，不能偷看

    playRound(s, setParam("bin", "value", 120), tuningEval(0, 0), true);
    EXPECT_FALSE(s.recordBlindEvaluation(tuningEval(0, 0)).ok());  // 必须正好是盲测集
    ASSERT_TRUE(s.recordBlindEvaluation(blindEval(1, 0)).ok());
    EXPECT_EQ(s.recordBlindEvaluation(blindEval(0, 0)).error().code, "assistant.blind_used");  // 不能“再测一次直到满意”
    EXPECT_FALSE(s.beginRound(0).ok());
    EXPECT_EQ(s.state().blind->misses, 1);
}

// A8：只有会话结束并且有了报告，才接受“采纳”
TEST(AssistSession, A8_AdoptOnlyAfterReport) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    EXPECT_EQ(s.adopt().error().code, "assistant.not_ended");
    playRound(s, setParam("bin", "value", 120), tuningEval(0, 0), true);
    EXPECT_EQ(s.adopt().error().code, "assistant.no_report");
    ASSERT_TRUE(s.recordBlindEvaluation(blindEval(0, 0)).ok());
    ASSERT_TRUE(s.generateReport(1.0).ok());
    (void)s.pullEvents();
    ASSERT_TRUE(s.adopt().ok());
    EXPECT_EQ(s.status(), SessionStatus::Adopted);
    EXPECT_EQ(eventNames(s), std::vector<std::string>{"CandidateAdopted"});
    EXPECT_FALSE(s.adopt().ok());
    EXPECT_FALSE(s.abandon().ok());
}

TEST(AssistSession, AbandonAnyTimeBeforeDecision) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    ASSERT_TRUE(s.abandon().ok());  // 进行中也可以放弃
    EXPECT_EQ(s.status(), SessionStatus::Abandoned);
    EXPECT_EQ(s.state().endReason, EndReason::Cancelled);
    EXPECT_FALSE(s.adopt().ok());
}

// A9：数据外发策略
TEST(AssistSession, A9_DataSharingPolicy) {
    CaseObservation o;
    o.caseId = "ok1";
    o.verdict = "NG";
    o.imageRef = "/secret/ok1.png";
    o.outputs["blobs.count"] = "number 3";

    {
        AssistSession s = newSession(kNoHardening, DataSharing::NoImages);
        const auto view = s.advisorView({o});
        ASSERT_EQ(view.observations.size(), 1u);
        EXPECT_TRUE(view.observations[0].imageRef.empty());                    // 图像不外发
        EXPECT_EQ(view.observations[0].outputs.at("blobs.count"), "number 3");  // 数字和判定可以
    }
    {
        AssistSession s = newSession(kNoHardening, DataSharing::Thumbnails);
        const auto view = s.advisorView({o});
        EXPECT_FALSE(view.observations[0].imageRef.empty());
        EXPECT_TRUE(view.observations[0].thumbnailOnly);
    }
    {
        AssistSession s = newSession(kNoHardening, DataSharing::Originals);
        const auto view = s.advisorView({o});
        EXPECT_FALSE(view.observations[0].imageRef.empty());
        EXPECT_FALSE(view.observations[0].thumbnailOnly);
    }
}

// ---------------- 报告 ----------------

TEST(AssistReport, FlagsCandidateThatOnlyFitsTuningCases) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    playRound(s, setParam("bin", "value", 120), tuningEval(0, 0), true);
    ASSERT_TRUE(s.recordBlindEvaluation(blindEval(1, 0)).ok());  // 盲测集上漏检
    ASSERT_TRUE(s.generateReport(2.0).ok());

    const AssistReport& r = *s.state().report;
    EXPECT_TRUE(r.tuning.meetsCriteria);
    EXPECT_TRUE(r.overfitSuspected);
    EXPECT_FALSE(r.notes.empty());
    EXPECT_EQ(r.initialValues.at("bin.value"), 20);
    EXPECT_EQ(r.finalValues.at("bin.value"), 120);
    EXPECT_EQ(r.rounds, 1);
    EXPECT_EQ(r.kept, 1);
    EXPECT_FALSE(s.generateReport(2.0).ok());
}

TEST(AssistReport, NoOverfitFlagWhenBlindIsAsGood) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    playRound(s, setParam("bin", "value", 120), tuningEval(0, 0), true);
    ASSERT_TRUE(s.recordBlindEvaluation(blindEval(0, 0)).ok());
    ASSERT_TRUE(s.recordRobustness({{"brightness", 0.8, 4, 0, 0, 0, true}}).ok());
    ASSERT_TRUE(s.generateReport(2.0).ok());
    EXPECT_FALSE(s.state().report->overfitSuspected);
}

TEST(AssistReport, SaysSoWhenThereIsNoBlindSetOrCriteriaNotMet) {
    InspectionBrief b = brief();
    b.blindCaseIds.clear();
    auto created = AssistSession::start("s", b, Budget{1, 100, 0}, brightSpot(), "x", kTuning, "r");
    ASSERT_TRUE(created.ok());
    AssistSession s = std::move(created).value();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    playRound(s, setParam("bin", "value", 100), tuningEval(0, 1), true);
    EXPECT_FALSE(s.beginRound(0).ok());
    EXPECT_FALSE(s.recordBlindEvaluation(blindEval(0, 0)).ok());
    ASSERT_TRUE(s.generateReport(1.0).ok());
    const AssistReport& r = *s.state().report;
    EXPECT_EQ(r.endReason, EndReason::BudgetExhausted);
    EXPECT_FALSE(r.tuning.meetsCriteria);
    EXPECT_FALSE(r.blind.has_value());
    EXPECT_GE(r.notes.size(), 3u);  // 没达标、没有盲测集、没做稳健性检查……都要说
}

TEST(AssistSession, RestoreKeepsEverything) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    playRound(s, setParam("bin", "value", 100), tuningEval(0, 1), true);
    AssistSession copy = AssistSession::restore(s.state(), brightSpot());
    EXPECT_EQ(copy.state().rounds.size(), 1u);
    EXPECT_EQ(copy.state().bestValues.at("bin.value"), 100);
    EXPECT_TRUE(copy.beginRound(0).ok());
}

// ---------------- 盲测集的划分 ----------------

TEST(ChooseBlindCases, StratifiedAndReproducible) {
    std::vector<std::pair<std::string, bool>> cases;
    for (int i = 0; i < 10; ++i) cases.emplace_back("ok" + std::to_string(i), false);
    for (int i = 0; i < 4; ++i) cases.emplace_back("ng" + std::to_string(i), true);

    const auto blind = chooseBlindCases(cases, 0.3, 7);
    const auto count = [&blind](const char* prefix) {
        return std::count_if(blind.begin(), blind.end(), [prefix](const std::string& s) { return s.rfind(prefix, 0) == 0; });
    };
    EXPECT_EQ(count("ok"), 3);
    EXPECT_EQ(count("ng"), 1);

    EXPECT_EQ(chooseBlindCases(cases, 0.3, 7), blind);  // 同样的种子，同样的结果
    auto reversed = cases;
    std::reverse(reversed.begin(), reversed.end());
    EXPECT_EQ(chooseBlindCases(reversed, 0.3, 7), blind);  // 和输入的顺序无关

    bool differs = false;
    for (unsigned seed = 8; seed < 20 && !differs; ++seed) differs = chooseBlindCases(cases, 0.3, seed) != blind;
    EXPECT_TRUE(differs);  // 换种子会换一种划分
}

TEST(ChooseBlindCases, AlwaysLeavesSomethingToTuneOn) {
    EXPECT_TRUE(chooseBlindCases({{"a", false}, {"b", true}}, 0.5, 1).empty());  // 每类只有一个：都留给调参
    const auto blind = chooseBlindCases({{"a", false}, {"b", false}, {"c", true}, {"d", true}}, 1.0, 1);
    EXPECT_EQ(blind.size(), 2u);  // 比例再大，每类也至少留一个调参
    EXPECT_TRUE(chooseBlindCases({{"a", false}, {"b", false}}, 0.0, 1).empty());
}

// ---------------- 加固阶段 ----------------
// 工作流 brightSpot() 声明了一项稳健性检查：亮度 ×0.8 和 ×1.2。

namespace {

// 稳健性检查的结果：两个程度各判错几个
std::vector<RobustnessResult> robust(int wrongAt08, int wrongAt12) {
    return {{"brightness", 0.8, 4, wrongAt08, 0, 0, wrongAt08 == 0}, {"brightness", 1.2, 4, 0, wrongAt12, 0, wrongAt12 == 0}};
}

AssistSession hardeningSession(int hardeningRounds = 3) {
    auto s = AssistSession::start("s-h", brief(), Budget{20, 300, hardeningRounds}, brightSpot(), "x", kAll, "recipe-cand");
    EXPECT_TRUE(s.ok());
    return std::move(s).value();
}

// 走到“刚达标”：稳健性检查里还有 wrong 个判错
void reachCriteria(AssistSession& s, int wrongAt08, int wrongAt12) {
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 180)).ok());
    ASSERT_TRUE(s.recordEvaluation(tuningEval(0, 0), Comparison::Better, robust(wrongAt08, wrongAt12), 0.01).ok());
}

}  // namespace

TEST(Hardening, NeedsRobustnessOnlyWhenCriteriaMetAndEnabled) {
    AssistSession on = hardeningSession();
    EXPECT_TRUE(on.needsRobustness(tuningEval(0, 0)));
    EXPECT_FALSE(on.needsRobustness(tuningEval(0, 1)));  // 没达标：不值得做
    AssistSession off = newSession(kNoHardening);
    EXPECT_FALSE(off.needsRobustness(tuningEval(0, 0)));  // 加固的预算是 0
}

// 达标之后不立即结束：稳健性检查里还有判错的，进入加固阶段
TEST(Hardening, SessionContinuesAfterCriteriaMetWhileProbesFail) {
    AssistSession s = hardeningSession();
    reachCriteria(s, 2, 0);
    EXPECT_EQ(s.status(), SessionStatus::InProgress);
    EXPECT_TRUE(s.hardening());
    EXPECT_EQ(s.state().valuesWhenFirstMet.at("bin.value"), 180);

    const auto view = s.advisorView({});
    EXPECT_TRUE(view.hardening);  // 顾问知道现在的目标变了
    ASSERT_EQ(view.robustness.size(), 2u);
    EXPECT_EQ(view.robustness[0].misses, 2);
    EXPECT_EQ(view.roundsLeft, 3);  // 剩下的是加固的预算
    EXPECT_EQ(view.history.back().robustnessWrong, 2);
}

// 稳健性检查全部通过：没有什么可以再加固的，直接结束
TEST(Hardening, EndsImmediatelyWhenAllProbesPass) {
    AssistSession s = hardeningSession();
    reachCriteria(s, 0, 0);
    EXPECT_EQ(s.status(), SessionStatus::Ended);
    EXPECT_EQ(s.state().endReason, EndReason::CriteriaMet);
    EXPECT_EQ(s.state().hardeningRounds, 0);
}

// A11：调参集一样好、稳健性更好 → 保留；稳健性没有更好 → 回退；调参集变差 → 回退，不管稳健性如何
TEST(Hardening, A11_KeepsOnlyChangesThatAreMoreRobustWithoutHurtingTuning) {
    AssistSession s = hardeningSession(5);
    reachCriteria(s, 2, 1);

    // 更稳：保留
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 170)).ok());
    auto rollback = s.recordEvaluation(tuningEval(0, 0), Comparison::Equal, robust(1, 1), 0.01);
    ASSERT_TRUE(rollback.ok());
    EXPECT_TRUE(rollback.value().empty());
    EXPECT_EQ(s.state().bestValues.at("bin.value"), 170);
    EXPECT_TRUE(s.state().rounds.back().hardening);

    // 一样稳：不保留（避免参数无意义地漂移）
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 165)).ok());
    rollback = s.recordEvaluation(tuningEval(0, 0), Comparison::Equal, robust(1, 1), 0.01);
    ASSERT_TRUE(rollback.ok());
    EXPECT_FALSE(rollback.value().empty());
    EXPECT_EQ(s.state().bestValues.at("bin.value"), 170);

    // 调参集变差了（不再达标，所以没有稳健性检查的结果）：回退
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 120)).ok());
    rollback = s.recordEvaluation(tuningEval(0, 1), Comparison::Worse, std::nullopt, 0.01);
    ASSERT_TRUE(rollback.ok());
    EXPECT_EQ(s.state().bestValues.at("bin.value"), 170);
    EXPECT_TRUE(s.state().best->meetsCriteria);

    // 全部通过：结束
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 175)).ok());
    ASSERT_TRUE(s.recordEvaluation(tuningEval(0, 0), Comparison::Equal, robust(0, 0), 0.01).ok());
    EXPECT_EQ(s.status(), SessionStatus::Ended);
    EXPECT_EQ(s.state().endReason, EndReason::CriteriaMet);
    EXPECT_EQ(s.state().hardeningRounds, 4);
}

// 加固有自己的预算；用完之后以“达标”结束，交出目前最稳的候选方案
TEST(Hardening, StopsWhenHardeningBudgetIsUsedUp) {
    AssistSession s = hardeningSession(2);
    reachCriteria(s, 2, 0);
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(s.beginRound(0).ok());
        ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 150 + i)).ok());
        ASSERT_TRUE(s.recordEvaluation(tuningEval(0, 0), Comparison::Equal, robust(2, 0), 0.01).ok());
    }
    EXPECT_EQ(s.beginRound(0).error().code, "assistant.hardening_done");
    EXPECT_EQ(s.status(), SessionStatus::Ended);
    EXPECT_EQ(s.state().endReason, EndReason::CriteriaMet);  // 不是“预算用尽”：方案是达标的
    EXPECT_EQ(s.state().bestValues.at("bin.value"), 180);
}

// 总预算在加固阶段用尽：结果仍然是“达标”
TEST(Hardening, TotalBudgetRunningOutDuringHardeningStillCountsAsMet) {
    auto created = AssistSession::start("s", brief(), Budget{1, 300, 5}, brightSpot(), "x", kAll, "r");
    ASSERT_TRUE(created.ok());
    AssistSession s = std::move(created).value();
    reachCriteria(s, 2, 0);
    EXPECT_FALSE(s.beginRound(0).ok());
    EXPECT_EQ(s.state().endReason, EndReason::CriteriaMet);
}

// A4 对稳健性检查同样适用：达标时必须给出，而且要和工作流声明的一一对应
TEST(Hardening, A4_RobustnessMustBeGivenAndMatchTheDeclaredProbes) {
    AssistSession s = hardeningSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 180)).ok());

    const auto submit = [&s](std::optional<std::vector<RobustnessResult>> r, const Evaluation& e) {
        return s.recordEvaluation(e, Comparison::Better, std::move(r), 0.01);
    };
    EXPECT_EQ(submit(std::nullopt, tuningEval(0, 0)).error().code, "assistant.robustness_missing");
    EXPECT_EQ(submit(robust(0, 0), tuningEval(0, 1)).error().code, "assistant.robustness_unexpected");  // 没达标却给了
    auto partial = robust(0, 0);
    partial.pop_back();
    EXPECT_EQ(submit(partial, tuningEval(0, 0)).error().code, "assistant.robustness_coverage");  // 少了一个程度
    auto wrongLevel = robust(0, 0);
    wrongLevel[1].level = 1.1;
    EXPECT_EQ(submit(wrongLevel, tuningEval(0, 0)).error().code, "assistant.robustness_coverage");
    auto fewerCases = robust(0, 0);
    fewerCases[0].total = 3;  // 没有覆盖整个调参集
    EXPECT_EQ(submit(fewerCases, tuningEval(0, 0)).error().code, "assistant.robustness_coverage");
    EXPECT_TRUE(submit(robust(1, 0), tuningEval(0, 0)).ok());
}

// 初始参数就达标：直接进入加固阶段，而不是结束
TEST(Hardening, BaselineThatAlreadyMeetsCriteriaGoesStraightToHardening) {
    AssistSession s = hardeningSession();
    EXPECT_EQ(s.recordBaseline(tuningEval(0, 0)).error().code, "assistant.robustness_missing");
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 0), robust(1, 0)).ok());
    EXPECT_TRUE(s.hardening());
    ASSERT_TRUE(s.beginRound(0).ok());
    EXPECT_TRUE(s.state().rounds.back().hardening);
}

TEST(Hardening, ReportComparesBeforeAndAfter) {
    AssistSession s = hardeningSession(1);
    reachCriteria(s, 2, 1);
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 170)).ok());
    ASSERT_TRUE(s.recordEvaluation(tuningEval(0, 0), Comparison::Equal, robust(1, 0), 0.01).ok());
    EXPECT_FALSE(s.beginRound(0).ok());  // 加固的预算用完
    ASSERT_TRUE(s.recordBlindEvaluation(blindEval(0, 0)).ok());
    ASSERT_TRUE(s.generateReport(1.0).ok());

    const AssistReport& r = *s.state().report;
    EXPECT_EQ(r.hardeningRounds, 1);
    EXPECT_EQ(r.valuesWhenFirstMet.at("bin.value"), 180);
    EXPECT_EQ(r.finalValues.at("bin.value"), 170);
    ASSERT_EQ(r.robustnessWhenFirstMet.size(), 2u);
    EXPECT_EQ(r.robustnessWhenFirstMet[0].misses, 2);
    ASSERT_EQ(r.robustness.size(), 2u);  // 最终的稳健性结果就是最好的候选方案的那一份，不用再测一遍
    EXPECT_EQ(r.robustness[0].misses, 1);
    bool mentioned = false;
    for (const auto& n : r.notes) mentioned |= n.find("加固") != std::string::npos;
    EXPECT_TRUE(mentioned);
}

// 会话在一轮进行中结束（例如顾问出错）：这一轮记为“未完成”，不算被拒绝
TEST(AssistSession, RoundOpenWhenSessionEndsIsRecordedAsAbandoned) {
    AssistSession s = newSession();
    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.end(EndReason::Failed).ok());
    EXPECT_EQ(s.state().rounds[0].outcome, RoundOutcome::Abandoned);
    ASSERT_TRUE(s.generateReport(1.0).ok());
    EXPECT_EQ(s.state().report->rejected, 0);
}

// “更稳”的规则：先比通不过的扰动有几项，再比判错的总数，再比漏检
TEST(Hardening, MoreRobustComparesFailedChecksFirst) {
    const auto r = [](int misses, int falseAlarms) {
        return RobustnessResult{"brightness", 1.0, 16, misses, falseAlarms, 0, misses + falseAlarms == 0};
    };
    // 1 项不过（8 个漏检）比 3 项不过（各 1 个误报）更稳：能用的范围更大。
    // 如果按“漏检优先”来比，结论会反过来——那正是这条规则要避免的。
    EXPECT_TRUE(moreRobust({r(8, 0), r(0, 0), r(0, 0)}, {r(0, 1), r(0, 1), r(0, 1)}));
    EXPECT_FALSE(moreRobust({r(0, 1), r(0, 1), r(0, 1)}, {r(8, 0), r(0, 0), r(0, 0)}));
    // 不过的项数一样：判错总数少的更稳
    EXPECT_TRUE(moreRobust({r(2, 0), r(0, 0)}, {r(5, 0), r(0, 0)}));
    // 总数也一样：漏检少的更稳
    EXPECT_TRUE(moreRobust({r(0, 3), r(0, 0)}, {r(3, 0), r(0, 0)}));
    // 完全一样：不算更稳
    EXPECT_FALSE(moreRobust({r(1, 0), r(0, 0)}, {r(1, 0), r(0, 0)}));
}

// 顾问在加固阶段建议结束：方案是达标的，结束原因是“达标”而不是“顾问建议结束”
TEST(Hardening, AdvisorFinishingDuringHardeningCountsAsMet) {
    AssistSession s = hardeningSession();
    reachCriteria(s, 2, 0);
    ASSERT_TRUE(s.beginRound(0).ok());
    Proposal done;
    done.finish = true;
    ASSERT_TRUE(s.submitProposal(done).ok());
    EXPECT_EQ(s.state().endReason, EndReason::CriteriaMet);
}

// A3 在加固阶段同样成立：扰动之后的表现也只给调参集的，而且只在加固阶段给
TEST(Hardening, A3_ProbeObservationsExcludeBlindCases) {
    AssistSession s = hardeningSession();
    const auto probeOf = [](const std::string& id) {
        ProbeObservation p;
        p.kind = "brightness";
        p.level = 0.8;
        p.observation.caseId = id;
        p.observation.imageRef = "/img/" + id + ".png";
        return p;
    };
    const std::vector<ProbeObservation> probes = {probeOf("ng1"), probeOf("ng3"), probeOf("stranger")};  // ng3 属于盲测集

    ASSERT_TRUE(s.recordBaseline(tuningEval(0, 2)).ok());
    EXPECT_TRUE(s.advisorView({}, probes).probeObservations.empty());  // 还没达标：不是加固阶段，不给

    ASSERT_TRUE(s.beginRound(0).ok());
    ASSERT_TRUE(s.submitProposal(setParam("bin", "value", 180)).ok());
    ASSERT_TRUE(s.recordEvaluation(tuningEval(0, 0), Comparison::Better, robust(2, 0), 0.01).ok());
    const auto view = s.advisorView({}, probes);
    ASSERT_EQ(view.probeObservations.size(), 1u);
    EXPECT_EQ(view.probeObservations[0].observation.caseId, "ng1");
    EXPECT_TRUE(view.probeObservations[0].observation.imageRef.empty());
}

// ---------------- 模板槽和任务书里的模板（A12）----------------

namespace {

Result<Workflow> withSlots(std::vector<TemplateSlot> slots, std::vector<Tunable> tunables = {{"bin", "value", 0, 255, 20, ""}}) {
    return Workflow::create("wf", 1, "定位", "",
                            {{"src", "vision.source", {}}, {"gray", "vision.gray", {}}, {"match", "vision.match", {}},
                             {"bin", "vision.threshold", {}}},
                            {{"src.image", "gray.image"}, {"gray.image", "match.image"}}, std::move(tunables), {}, std::move(slots));
}

const TemplateSlot kLocator{"locator", "match", "template", "gray.image", "框一个角"};

Result<AssistSession> startWith(std::vector<TemplateChoice> templates, std::vector<TemplateSlot> slots = {kLocator}) {
    InspectionBrief b = brief();
    b.templates = std::move(templates);
    return AssistSession::start("s", b, kNoHardening, withSlots(std::move(slots)).value(), "x", kAll, "r");
}

}  // namespace

TEST(TemplateSlot, WorkflowValidatesItsSlots) {
    EXPECT_TRUE(withSlots({kLocator}).ok());
    const auto rejected = [](TemplateSlot slot) { return !withSlots({std::move(slot)}).ok(); };
    EXPECT_TRUE(rejected({"", "match", "template", "gray.image", ""}));          // 没有名字
    EXPECT_TRUE(rejected({"locator", "nope", "template", "gray.image", ""}));    // 节点不存在
    EXPECT_TRUE(rejected({"locator", "match", "", "gray.image", ""}));           // 没有参数名
    EXPECT_TRUE(rejected({"locator", "match", "template", "gray", ""}));         // 图像来源没有写端口
    EXPECT_TRUE(rejected({"locator", "match", "template", "nope.image", ""}));   // 图像来源的节点不存在
    EXPECT_FALSE(withSlots({kLocator, kLocator}).ok());                           // 重名
    EXPECT_FALSE(withSlots({kLocator, {"other", "match", "template", "gray.image", ""}}).ok());  // 两个槽指向同一个参数
    // 模板由人定：模板参数不能同时声明成可调项
    EXPECT_FALSE(withSlots({kLocator}, {{"match", "template", 0, 10, 1, ""}}).ok());
}

// A12：模板只能取自调参集；每个槽正好一个
TEST(TemplateSlot, A12_BriefMustProvideEachTemplateFromATuningCase) {
    EXPECT_TRUE(startWith({{"locator", "ok1", 10, 10, 40, 30}}).ok());
    EXPECT_EQ(startWith({{"locator", "ok3", 10, 10, 40, 30}}).error().code, "assistant.template_from_blind");  // ok3 在盲测集里
    EXPECT_EQ(startWith({{"locator", "ng3", 10, 10, 40, 30}}).error().code, "assistant.template_from_blind");
    EXPECT_EQ(startWith({{"locator", "zzz", 10, 10, 40, 30}}).error().code, "assistant.template_case");
    EXPECT_EQ(startWith({}).error().code, "assistant.missing_template");
    EXPECT_EQ(startWith({{"other", "ok1", 10, 10, 40, 30}}).error().code, "assistant.unknown_template_slot");
    EXPECT_EQ(startWith({{"locator", "ok1", 10, 10, 40, 30}, {"locator", "ok2", 10, 10, 40, 30}}).error().code,
              "assistant.duplicate_template");
    EXPECT_EQ(startWith({{"locator", "ok1", 10, 10, 0, 30}}).error().code, "assistant.template_box");
    EXPECT_EQ(startWith({{"locator", "ok1", -1, 10, 40, 30}}).error().code, "assistant.template_box");
    // 没有模板槽的工作流：不需要、也不接受模板
    EXPECT_TRUE(startWith({}, {}).ok());
    EXPECT_EQ(startWith({{"locator", "ok1", 10, 10, 40, 30}}, {}).error().code, "assistant.unknown_template_slot");
}

// 顾问想改模板参数：它不是可调项，按 A10 拒绝
TEST(TemplateSlot, TemplateParamIsNotTunable) {
    const Workflow w = withSlots({kLocator}).value();
    EXPECT_EQ(w.validate(setParam("match", "template", 2)).error().code, "assistant.not_tunable");
}
