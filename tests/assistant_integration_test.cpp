// 方案助手的集成测试（POC-11）：真实算子 + 检测引擎 + 方案编排 + 上线管理的评估规则 + JSON 文件存储。
// 顾问是“按脚本出牌的顾问”：整个闭环里没有大模型，也没有网络。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <set>

#include "vision/assist_adapters/platform_ports.h"
#include "vision/assist_adapters/scripted_advisor.h"
#include "vision/assist_adapters/search_advisor.h"
#include "vision/assist_adapters/storage.h"
#include "vision/assistant/api.h"
#include "vision/authoring/api.h"
#include "vision/catalog/api.h"
#include "vision/common/path.h"
#include "vision/content/content_store.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/execution/api.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/llm/llm_advisor.h"
#include "vision/persistence/json_repositories.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/release/api.h"

using namespace vision;
using namespace vision::assistant;
using assist_adapters::ScriptedAdvisor;
namespace fs = std::filesystem;

namespace {

// 小图：浅灰背景上一块深色零件，spots 个亮斑；dust 为真时有几个 2×2 的灰尘亮点（不算缺陷）
cv::Mat drawProduct(int spots, bool dust, double lighting) {
    cv::Mat m(120, 160, CV_8UC3, cv::Scalar(150, 150, 150));
    cv::rectangle(m, cv::Rect(20, 20, 120, 80), cv::Scalar(70, 70, 70), cv::FILLED);
    for (int i = 0; i < spots; ++i) cv::circle(m, cv::Point(45 + 35 * i, 60), 5, cv::Scalar(235, 235, 235), cv::FILLED);
    if (dust) {
        cv::rectangle(m, cv::Rect(30, 30, 2, 2), cv::Scalar(240, 240, 240), cv::FILLED);
        cv::rectangle(m, cv::Rect(120, 85, 2, 2), cv::Scalar(240, 240, 240), cv::FILLED);
    }
    m.convertTo(m, -1, lighting, 0);
    return m;
}

const char* kWorkflow = R"({
  "id": "bright-spot", "name": "亮斑检测", "description": "灰度 → 阈值 → 连通域 → 数量判定",
  "nodes": [
    {"key": "input", "operator": "vision.image_input"},
    {"key": "gray", "operator": "vision.to_gray"},
    {"key": "bin", "operator": "vision.threshold", "params": {"method": "fixed"}},
    {"key": "blobs", "operator": "vision.blob"},
    {"key": "judge", "operator": "vision.judge_count", "params": {"maxCount": 0}}
  ],
  "edges": [["input.image", "gray.image"], ["gray.image", "bin.image"], ["bin.region", "blobs.region"],
            ["blobs.blobs", "judge.blobs"]],
  "tunables": [
    {"node": "bin", "param": "value", "min": 0, "max": 255, "initial": 100},
    {"node": "blobs", "param": "minArea", "min": 1, "max": 2000, "initial": 1},
    {"node": "blobs", "param": "maxArea", "min": -100, "max": 1000000000, "initial": 1000000000}
  ],
  "robustness": [{"kind": "brightness", "levels": [0.8, 1.1]}, {"kind": "noise", "levels": [8]}]
})";

Proposal setParam(const std::string& node, const std::string& param, double value, const std::string& why = "") {
    Proposal p;
    p.changes.push_back({ProposedChange::Kind::SetParam, node, param, value});
    p.rationale = why;
    return p;
}

class AssistantIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root = fs::temp_directory_path() / (std::string("vision-assist-") + info->name());
        fs::remove_all(root);
        fs::create_directories(root / "images");

        recipes = std::make_unique<persistence::JsonRecipeRepository>(root);
        versions = std::make_unique<persistence::JsonRecipeVersionRepository>(root);
        goldenRepository = std::make_unique<persistence::JsonGoldenSetRepository>(root);
        authoring = std::make_unique<authoring::AuthoringService>(*recipes, *versions, catalog::OperatorCatalog::global(), bus);
        execution = std::make_unique<execution::ExecutionService>(catalog::OperatorCatalog::global(), bus);
        golden = std::make_unique<release::GoldenSetService>(*goldenRepository, bus);
        workflows = std::make_unique<assist_adapters::JsonWorkflowRepository>(root);
        sessions = std::make_unique<assist_adapters::JsonAssistSessionRepository>(root);
        editor = std::make_unique<assist_adapters::AuthoringCandidateEditor>(*authoring);
        runner = std::make_unique<assist_adapters::EngineTrialRunner>(*authoring, *execution);
        goldenCases = std::make_unique<assist_adapters::ReleaseGoldenCases>(*golden);
        // 匹配模板：方案编排往内容仓库里存，检测引擎从内容仓库里取
        contentStore = std::make_unique<content::FileContentStore>(root);
        templateContent = std::make_unique<content::TemplateContent>(*contentStore);
        authoring->useTemplateStore(*templateContent);
        execution->useTemplateSource(*templateContent);

        bus.subscribe("*", [this](const EventPtr& e) { events.emplace_back(e->name()); });

        const fs::path file = root / "bright-spot.json";
        std::ofstream(file, std::ios::binary) << kWorkflow;
        ASSERT_TRUE(workflows->importFile(file).ok());
    }
    void TearDown() override { fs::remove_all(root); }

    // 16 个调参用例（正常光照）+ 8 个盲测用例（光照倍数为 blindLighting）
    AssistRequest makeRequest(double blindLighting) {
        release::GoldenSetDefinition def;
        def.name = "亮斑";
        AssistRequest request;
        const auto add = [&](const std::string& id, int spots, bool dust, double lighting, bool blind) {
            const std::string file = pathToUtf8(root / "images" / (id + ".png"));
            EXPECT_TRUE(imaging::saveImage(file, imaging::fromMat(drawProduct(spots, dust, lighting)).value()).ok());
            def.cases.push_back({id, file, spots > 0, {}});
            if (blind) request.blindCaseIds.push_back(id);
        };
        for (int i = 0; i < 8; ++i) {
            add("ok" + std::to_string(i), 0, i % 2 == 0, 1.0, false);
            add("ng" + std::to_string(i), 1 + i % 3, false, 1.0, false);
        }
        for (int i = 0; i < 4; ++i) {
            add("blind_ok" + std::to_string(i), 0, i % 2 == 0, blindLighting, true);
            add("blind_ng" + std::to_string(i), 1 + i % 3, false, blindLighting, true);
        }
        auto id = golden->createGoldenSet(def);
        EXPECT_TRUE(id.ok());
        request.hardeningRounds = 0;  // 这些测试关心的是调参：达标就结束。加固另有专门的测试
        request.goal = "检出亮斑";
        request.goldenSetId = id.value();
        request.workflowId = "bright-spot";
        return request;
    }

    Result<SessionView> run(ScriptedAdvisor& advisor, const AssistRequest& request) {
        AssistService service(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
        return service.run(request);
    }

    // 候选方案草稿里某个算子的某个参数现在的值
    double draftParam(const std::string& recipeId, const std::string& operatorId, const std::string& param) {
        const auto draft = authoring->getDraft(recipeId);
        EXPECT_TRUE(draft.ok());
        for (const auto& n : draft.value().nodes) {
            if (n.operatorId == operatorId) return std::get<double>(n.params.at(param));
        }
        ADD_FAILURE() << "草稿里没有 " << operatorId;
        return -1;
    }

    int count(const std::string& eventName) const {
        return static_cast<int>(std::count(events.begin(), events.end(), eventName));
    }

    fs::path root;
    eventbus::InMemoryEventBus bus;
    std::unique_ptr<persistence::JsonRecipeRepository> recipes;
    std::unique_ptr<persistence::JsonRecipeVersionRepository> versions;
    std::unique_ptr<persistence::JsonGoldenSetRepository> goldenRepository;
    std::unique_ptr<authoring::AuthoringService> authoring;
    std::unique_ptr<execution::ExecutionService> execution;
    std::unique_ptr<release::GoldenSetService> golden;
    std::unique_ptr<assist_adapters::JsonWorkflowRepository> workflows;
    std::unique_ptr<assist_adapters::JsonAssistSessionRepository> sessions;
    std::unique_ptr<assist_adapters::AuthoringCandidateEditor> editor;
    std::unique_ptr<assist_adapters::EngineTrialRunner> runner;
    std::unique_ptr<assist_adapters::ReleaseGoldenCases> goldenCases;
    std::unique_ptr<content::FileContentStore> contentStore;
    std::unique_ptr<content::TemplateContent> templateContent;
    std::vector<std::string> events;
};

// 一份“好”的脚本：从故意很差的初始参数（阈值 100，背景都被当成亮斑）调到达标
std::vector<Proposal> goodScript() {
    return {setParam("bin", "value", 140, "先提高一点"),       // 没有改善 → 回退
            setParam("bin", "value", 195, "背景比 140 亮"),     // 误报减少 → 保留
            setParam("blobs", "minArea", 30, "去掉灰尘")};      // 达标
}

}  // namespace

// 通过标准 2：从故意很差的初始参数出发，把候选方案调到在调参集上达标
TEST_F(AssistantIntegration, TunesBadInitialParametersUntilCriteriaMet) {
    ScriptedAdvisor advisor("scripted:test", goodScript());
    const auto request = makeRequest(1.0);
    auto result = run(advisor, request);
    ASSERT_TRUE(result.ok()) << result.error().message;
    const SessionView& v = result.value();

    EXPECT_EQ(v.status, "ended");
    EXPECT_EQ(v.endReason, "criteria-met");
    EXPECT_EQ(v.rounds, 3);
    EXPECT_EQ(v.kept, 2);
    EXPECT_EQ(v.rolledBack, 1);
    EXPECT_TRUE(v.tuningMeetsCriteria);
    ASSERT_TRUE(v.blindMeetsCriteria.has_value());
    EXPECT_TRUE(*v.blindMeetsCriteria);  // 盲测集和调参集光照一样：也达标
    EXPECT_FALSE(v.overfitSuspected);

    // 候选方案的草稿确实停在了最好的参数上
    EXPECT_EQ(draftParam(v.candidateRecipeId, "vision.threshold", "value"), 195);
    EXPECT_EQ(draftParam(v.candidateRecipeId, "vision.blob", "minArea"), 30);

    // A1：助手没有发布任何东西
    EXPECT_EQ(count("RecipeVersionPublished"), 0);
    EXPECT_EQ(count("AssistSessionStarted"), 1);
    EXPECT_EQ(count("ExperimentCompleted"), 3);
    EXPECT_EQ(count("BlindEvaluationCompleted"), 1);
    EXPECT_EQ(count("AssistReportGenerated"), 1);

    // 报告里有稳健性检查和每一轮的记录
    EXPECT_NE(v.reportMarkdown.find("稳健性检查"), std::string::npos);
    EXPECT_NE(v.reportMarkdown.find("bin.value = 195"), std::string::npos);
}

// A3 + A9：顾问从头到尾没有见过盲测集，也没有拿到任何图像（数据外发策略是“不外发图像”）
TEST_F(AssistantIntegration, AdvisorSeesNoBlindCasesAndNoImages) {
    ScriptedAdvisor advisor("scripted:test", goodScript());
    const auto request = makeRequest(1.0);
    ASSERT_TRUE(run(advisor, request).ok());

    const std::set<std::string> blind(request.blindCaseIds.begin(), request.blindCaseIds.end());
    ASSERT_EQ(advisor.seen().size(), 3u);
    for (const auto& view : advisor.seen()) {
        EXPECT_EQ(view.observations.size(), 16u);
        for (const auto& o : view.observations) {
            EXPECT_EQ(blind.count(o.caseId), 0u) << o.caseId;
            EXPECT_TRUE(o.imageRef.empty());
        }
        ASSERT_TRUE(view.best.has_value());
        EXPECT_EQ(view.best->caseIds.size(), 16u);
    }
    // 但数字是给了的：顾问靠它们判断下一步怎么改。名字用的是工作流里节点的名字，不是内部的节点 ID
    const auto& first = advisor.seen().front().observations.front();
    EXPECT_EQ(first.outputs.count("blobs.count"), 1u);
    EXPECT_FALSE(first.verdict.empty());
}

// 通过标准 3：越界的提议（改不可调的参数、超出范围、改变结构）被拒绝并记录，候选方案不受影响
TEST_F(AssistantIntegration, IllegalProposalsAreRejectedAndRecorded) {
    Proposal addNode;
    addNode.changes.push_back({ProposedChange::Kind::AddNode, "invert", "", 0});
    Proposal sneaky = setParam("bin", "value", 195);  // 一处合法的修改里夹带一处不合法的
    sneaky.changes.push_back({ProposedChange::Kind::SetParam, "judge", "maxCount", 99});

    ScriptedAdvisor advisor("scripted:test", {setParam("judge", "maxCount", 5), setParam("bin", "value", 300), addNode, sneaky});
    auto result = run(advisor, makeRequest(1.0));
    ASSERT_TRUE(result.ok()) << result.error().message;
    const SessionView& v = result.value();

    EXPECT_EQ(v.rejected, 4);
    EXPECT_EQ(v.kept, 0);
    EXPECT_EQ(v.endReason, "advisor-finished");
    EXPECT_FALSE(v.tuningMeetsCriteria);
    EXPECT_EQ(count("ProposalRejected"), 4);

    // 候选方案还是初始参数；判定用的参数没有被动过
    EXPECT_EQ(draftParam(v.candidateRecipeId, "vision.threshold", "value"), 100);
    EXPECT_EQ(draftParam(v.candidateRecipeId, "vision.judge_count", "maxCount"), 0);

    // 记录里留下了每一份被拒绝的提议和原因
    auto stored = sessions->find(v.sessionId);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->state().rounds.size(), 5u);  // 4 份被拒绝的 + 最后一轮“顾问建议结束”
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(stored->state().rounds[i].outcome, RoundOutcome::RejectedByRules);
        EXPECT_FALSE(stored->state().rounds[i].rejectReason.empty());
    }
    // 报告如实写明没有达标
    EXPECT_NE(v.reportMarkdown.find("没有达到验收标准"), std::string::npos);
}

// 通过标准 4：只适应了调参集的候选方案，会被报告指出来。
// 盲测集的光照比调参集暗（×0.6）：调出来的阈值 195 在暗的图上一个亮斑都找不到。
TEST_F(AssistantIntegration, ReportFlagsCandidateThatOnlyFitsTuningCases) {
    ScriptedAdvisor advisor("scripted:test", goodScript());
    auto result = run(advisor, makeRequest(0.6));
    ASSERT_TRUE(result.ok()) << result.error().message;
    const SessionView& v = result.value();

    EXPECT_TRUE(v.tuningMeetsCriteria);  // 调参集上是达标的
    ASSERT_TRUE(v.blindMeetsCriteria.has_value());
    EXPECT_FALSE(*v.blindMeetsCriteria);
    EXPECT_TRUE(v.overfitSuspected);
    EXPECT_NE(v.reportMarkdown.find("不建议直接采纳"), std::string::npos);

    auto stored = sessions->find(v.sessionId);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->state().blind->misses, 4);
    // 稳健性检查（亮度 ×0.8）也提前暴露了同一个问题
    bool brightnessFailed = false;
    for (const auto& r : stored->state().report->robustness) brightnessFailed |= (r.kind == "brightness" && !r.meetsCriteria);
    EXPECT_TRUE(brightnessFailed);
}

// A2：方案编排拒绝编辑命令时如实记录，候选方案恢复原状。
// 工作流把 blobs.maxArea 的范围声明得比算子允许的还宽（最小 -100），所以 -5 能通过助手的检查，却会被方案编排拒绝。
TEST_F(AssistantIntegration, EditorRejectionIsRecordedAndCandidateRestored) {
    Proposal both = setParam("bin", "value", 195);
    both.changes.push_back({ProposedChange::Kind::SetParam, "blobs", "maxArea", -5});  // 第一处已经生效之后，第二处被拒绝
    ScriptedAdvisor advisor("scripted:test", {both});
    auto result = run(advisor, makeRequest(1.0));
    ASSERT_TRUE(result.ok()) << result.error().message;

    auto stored = sessions->find(result.value().sessionId);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->state().rounds[0].outcome, RoundOutcome::RejectedByEditor);
    EXPECT_FALSE(stored->state().rounds[0].rejectReason.empty());
    EXPECT_EQ(draftParam(result.value().candidateRecipeId, "vision.threshold", "value"), 100);  // 第一处修改被撤销了
}

// A6：预算用尽时结束，交出当时最好的候选方案
TEST_F(AssistantIntegration, StopsWhenBudgetIsExhausted) {
    ScriptedAdvisor advisor("scripted:test", goodScript());
    auto request = makeRequest(1.0);
    request.maxRounds = 2;
    auto result = run(advisor, request);
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().endReason, "budget-exhausted");
    EXPECT_EQ(result.value().rounds, 2);
    EXPECT_FALSE(result.value().tuningMeetsCriteria);
    EXPECT_EQ(draftParam(result.value().candidateRecipeId, "vision.threshold", "value"), 195);  // 第 2 轮的改进保留了下来
}

// 顾问出错（以后的大模型顾问可能断网、超时）：会话结束，不崩溃，交出目前最好的候选方案
TEST_F(AssistantIntegration, AdvisorFailureEndsSessionGracefully) {
    class FailingAdvisor final : public RecipeAdvisor {
    public:
        std::string name() const override { return "failing"; }
        Result<Proposal> propose(const AdvisorView&) override {
            if (++calls_ == 1) return setParam("bin", "value", 195);
            return Result<Proposal>::failure("advisor.unreachable", "模拟：连不上顾问");
        }

    private:
        int calls_ = 0;
    } advisor;
    AssistService service(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    auto result = service.run(makeRequest(1.0));
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().endReason, "failed");
    EXPECT_EQ(result.value().kept, 1);
    EXPECT_EQ(draftParam(result.value().candidateRecipeId, "vision.threshold", "value"), 195);
}

// A8：只有人可以采纳，而且采纳不等于发布
TEST_F(AssistantIntegration, AdoptionIsAHumanDecisionAndDoesNotPublish) {
    ScriptedAdvisor advisor("scripted:test", goodScript());
    AssistService service(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    auto result = service.run(makeRequest(1.0));
    ASSERT_TRUE(result.ok()) << result.error().message;
    const std::string id = result.value().sessionId;

    EXPECT_EQ(service.getSession(id).value().status, "ended");  // 跑完之后停在“等人决定”
    ASSERT_TRUE(service.adopt(id).ok());
    EXPECT_EQ(service.getSession(id).value().status, "adopted");
    EXPECT_FALSE(service.adopt(id).ok());
    EXPECT_FALSE(service.abandon(id).ok());
    EXPECT_EQ(count("CandidateAdopted"), 1);
    EXPECT_EQ(count("RecipeVersionPublished"), 0);

    // 采纳之后，由人来发布：候选方案是一份合法的、可以发布的草稿
    EXPECT_TRUE(authoring->publish(result.value().candidateRecipeId).ok());
    EXPECT_FALSE(service.adopt("no-such-session").ok());
}

// 会话记录存进文件再读出来，内容不变
TEST_F(AssistantIntegration, SessionRecordSurvivesSaveAndLoad) {
    ScriptedAdvisor advisor("scripted:test", goodScript());
    auto result = run(advisor, makeRequest(0.6));
    ASSERT_TRUE(result.ok()) << result.error().message;

    auto stored = sessions->find(result.value().sessionId);
    ASSERT_TRUE(stored.has_value());
    const SessionState& s = stored->state();
    EXPECT_EQ(s.rounds.size(), 3u);
    EXPECT_EQ(s.rounds[1].proposal.rationale, "背景比 140 亮");
    EXPECT_EQ(s.rounds[0].decision, RoundDecision::RolledBack);
    EXPECT_EQ(s.bestValues.at("bin.value"), 195);
    EXPECT_EQ(s.brief.blindCaseIds.size(), 8u);
    ASSERT_TRUE(s.report.has_value());
    EXPECT_TRUE(s.report->overfitSuspected);
    EXPECT_EQ(stored->workflow().tunables().size(), 3u);

    AssistService service(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    EXPECT_EQ(service.getSession(result.value().sessionId).value().reportMarkdown, result.value().reportMarkdown);
    EXPECT_EQ(service.listSessions().size(), 1u);

    const auto bytes = fs::file_size(sessions->fileOf(result.value().sessionId));
    std::cout << "[测量] 会话记录 " << bytes << " 字节（3 轮，24 个用例）；每轮耗时";
    for (double t : result.value().roundSeconds) std::cout << " " << t * 1000 << " ms";
    std::cout << "\n";
    EXPECT_LT(bytes, 100u * 1024u);
}

// 不指定盲测集时，按比例从基准图集里分层抽取
TEST_F(AssistantIntegration, BlindCasesAreChosenAutomaticallyWhenNotGiven) {
    ScriptedAdvisor advisor("scripted:test", goodScript());
    auto request = makeRequest(1.0);
    request.blindCaseIds.clear();
    request.blindFraction = 0.25;
    auto result = run(advisor, request);
    ASSERT_TRUE(result.ok()) << result.error().message;
    auto stored = sessions->find(result.value().sessionId);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->state().brief.blindCaseIds.size(), 6u);  // 12 合格 + 12 不合格，各抽 3 个
    EXPECT_EQ(stored->state().tuningCaseIds.size(), 18u);
}

TEST_F(AssistantIntegration, RejectsUnknownWorkflowOrGoldenSet) {
    ScriptedAdvisor advisor("scripted:test", {});
    auto request = makeRequest(1.0);
    auto bad = request;
    bad.workflowId = "nope";
    EXPECT_FALSE(run(advisor, bad).ok());
    bad = request;
    bad.goldenSetId = "nope";
    EXPECT_FALSE(run(advisor, bad).ok());
    bad = request;
    bad.dataSharing = "everything";
    EXPECT_FALSE(run(advisor, bad).ok());
}

// ---------------- 搜索顾问：不用大模型，也没有人告诉它答案 ----------------

// 从同样很差的初始参数出发，搜索顾问自己把候选方案调到达标
TEST_F(AssistantIntegration, SearchAdvisorFindsParametersByItself) {
    assist_adapters::SearchAdvisor advisor;
    AssistService service(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    auto request = makeRequest(1.0);
    request.maxRounds = 40;
    auto result = service.run(request);
    ASSERT_TRUE(result.ok()) << result.error().message;
    const SessionView& v = result.value();

    EXPECT_EQ(v.endReason, "criteria-met");
    EXPECT_TRUE(v.tuningMeetsCriteria);
    ASSERT_TRUE(v.blindMeetsCriteria.has_value());
    EXPECT_TRUE(*v.blindMeetsCriteria);  // 没有见过的用例上也达标
    EXPECT_FALSE(v.overfitSuspected);
    EXPECT_EQ(v.rejected, 0);            // 它的提议全部合法
    EXPECT_LE(v.rounds, 20);
    const double threshold = draftParam(v.candidateRecipeId, "vision.threshold", "value");
    EXPECT_GT(threshold, 150);  // 高于背景
    EXPECT_LT(threshold, 235);  // 低于亮斑
    std::cout << "[测量] 搜索顾问用了 " << v.rounds << " 轮（保留 " << v.kept << "），阈值 " << threshold << "，最小面积 "
              << draftParam(v.candidateRecipeId, "vision.blob", "minArea") << "\n";
}

namespace {

// 只测搜索顾问自己的逻辑：手工构造助手给它的内容，不运行任何检测
AdvisorView viewOf(std::vector<TunableState> tunables, std::vector<RoundSummary> history = {}) {
    AdvisorView v;
    v.tunables = std::move(tunables);
    v.history = std::move(history);
    return v;
}

TunableState tunable(const std::string& param, double min, double max, double value) {
    return {{"n", param, min, max, value, ""}, value, value};
}

RoundSummary round(int index, const std::string& decision) {
    RoundSummary r;
    r.index = index;
    r.outcome = "applied";
    r.decision = decision;
    return r;
}

}  // namespace

TEST(SearchAdvisor, TriesEvenlySpacedValuesAndSkipsTheCurrentBest) {
    assist_adapters::SearchAdvisor advisor(5, 1);
    const auto view = viewOf({tunable("p", 0, 100, 50)});
    std::vector<double> tried;
    std::vector<RoundSummary> history;
    for (int i = 0; i < 10; ++i) {
        auto p = advisor.propose(viewOf(view.tunables, history));
        ASSERT_TRUE(p.ok());
        if (p.value().finish) break;
        ASSERT_EQ(p.value().changes.size(), 1u);
        tried.push_back(p.value().changes[0].value);
        history.push_back(round(i + 1, "rolled-back"));
    }
    EXPECT_EQ(tried, (std::vector<double>{0, 25, 75, 100}));  // 50 是当前值，已经评估过，不再试
}

TEST(SearchAdvisor, UsesLogScaleForWideRanges) {
    assist_adapters::SearchAdvisor advisor(4, 1);
    auto p = advisor.propose(viewOf({tunable("area", 1, 1000, 1)}));
    ASSERT_TRUE(p.ok());
    EXPECT_DOUBLE_EQ(p.value().changes[0].value, 10);  // 1、10、100、1000；1 是当前值
}

TEST(SearchAdvisor, RefinesAroundTheBestValueAndStopsWhenNothingImproves) {
    assist_adapters::SearchAdvisor advisor(5, 3);
    std::vector<RoundSummary> history;
    std::vector<double> tried;
    double best = 50;
    for (int i = 0; i < 40; ++i) {
        auto p = advisor.propose(viewOf({tunable("p", 0, 100, best)}, history));
        ASSERT_TRUE(p.ok());
        if (p.value().finish) break;
        const double v = p.value().changes[0].value;
        tried.push_back(v);
        const bool keep = (v == 75);  // 假装只有 75 比原来好
        if (keep) best = v;
        history.push_back(round(i + 1, keep ? "kept" : "rolled-back"));
    }
    // 第 1 趟：0、25、75、100；第 2 趟在 75 左右各一格（50～100）里取 5 个点，75 本身不再试；没有改进，结束
    EXPECT_EQ(tried, (std::vector<double>{0, 25, 75, 100, 50, 62.5, 87.5, 100}));
}

TEST(SearchAdvisor, EveryProposalStaysWithinTheDeclaredRange) {
    assist_adapters::SearchAdvisor advisor(8, 3);
    const std::vector<TunableState> tunables = {tunable("a", 0, 255, 100), tunable("b", 1, 2000, 1), tunable("c", -5, 5, 0)};
    std::vector<RoundSummary> history;
    int proposals = 0;
    for (int i = 0; i < 200; ++i) {
        auto p = advisor.propose(viewOf(tunables, history));
        ASSERT_TRUE(p.ok());
        if (p.value().finish) break;
        ++proposals;
        const auto& c = p.value().changes[0];
        for (const auto& t : tunables) {
            if (t.tunable.param == c.param) {
                EXPECT_GE(c.value, t.tunable.min);
                EXPECT_LE(c.value, t.tunable.max);
            }
        }
        history.push_back(round(i + 1, "rolled-back"));
    }
    EXPECT_EQ(proposals, 23);  // 8 + 7 + 8：b 的当前值正好在网格上，少试一个。一趟没有改进就结束，不会没完没了
}

// ---------------- 大模型顾问接进整个闭环（POC-12）。大模型服务是假的：按顺序给出准备好的回复 ----------------

namespace {

class ScriptedLlmService final : public llm::HttpClient {
public:
    // 每一项是大模型“说”的话；用完之后表现为断网
    explicit ScriptedLlmService(std::vector<std::string> says) : says_(std::move(says)) {}
    Result<llm::HttpResponse> send(const llm::HttpRequest& request) override {
        bodies.push_back(request.body);
        if (next_ >= says_.size()) return Result<llm::HttpResponse>::failure("http.unreachable", "模拟：断网");
        const std::string body = nlohmann::json{
            {"model", "fake"},
            {"choices", nlohmann::json::array({{{"finish_reason", "stop"}, {"message", {{"content", says_[next_++]}}}}})}}
                                     .dump();
        return llm::HttpResponse{200, body};
    }
    std::vector<std::string> bodies;  // 发给“大模型”的全部内容

private:
    std::vector<std::string> says_;
    std::size_t next_ = 0;
};

llm::LlmAdvisor fakeLlmAdvisor(llm::HttpClient& http, llm::ImageLoader images = {}) {
    llm::LlmConfig config;
    config.model = "fake";
    config.maxRetries = 0;
    return llm::LlmAdvisor(config, llm::makeOpenAiCompatibleProtocol({"https://llm.example", "fake", "KEY", 1000, ""}), http,
                           std::move(images));
}

}  // namespace

// 大模型给出不合法的提议、甚至答非所问时，循环继续，不卡死、不崩溃；之后给出好的提议仍然能调到达标
TEST_F(AssistantIntegration, LlmAdvisor_BadRepliesDoNotBreakTheLoop) {
    ScriptedLlmService service({
        R"({"rationale": "放宽标准", "changes": [{"node": "judge", "param": "maxCount", "value": 9}]})",   // 不是可调项
        "我觉得这个方案挺好的，不用改。",                                                                   // 不是 JSON
        R"({"rationale": "调大", "changes": [{"node": "bin", "param": "value", "value": 999}]})",          // 超出范围
        R"({"rationale": "加滤波", "changes": [{"kind": "add-node", "node": "blur"}]})",                   // 改变结构
        R"({"rationale": "背景最亮约 150，亮斑约 235", "changes": [{"node": "bin", "param": "value", "value": 195},
                                                              {"node": "blobs", "param": "minArea", "value": 30}]})",
    });
    llm::LlmAdvisor advisor = fakeLlmAdvisor(service);
    AssistService assist(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    auto result = assist.run(makeRequest(1.0));
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().endReason, "criteria-met");
    EXPECT_EQ(result.value().rounds, 5);
    EXPECT_EQ(result.value().rejected, 4);
    EXPECT_EQ(result.value().advisor, "llm:openai:fake");

    // 被拒绝的原因在下一轮告诉了大模型
    EXPECT_NE(service.bodies[1].find("rejected"), std::string::npos);
}

// 大模型服务中途不可用：会话以“出错”结束，已有的改进和记录都保留
TEST_F(AssistantIntegration, LlmAdvisor_ServiceOutageEndsSessionAndKeepsTheRecord) {
    ScriptedLlmService service({R"({"rationale": "先提高阈值", "changes": [{"node": "bin", "param": "value", "value": 195}]})"});
    llm::LlmAdvisor advisor = fakeLlmAdvisor(service);
    AssistService assist(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    auto result = assist.run(makeRequest(1.0));
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().endReason, "failed");
    EXPECT_EQ(result.value().kept, 1);
    EXPECT_EQ(draftParam(result.value().candidateRecipeId, "vision.threshold", "value"), 195);

    auto stored = sessions->find(result.value().sessionId);
    ASSERT_TRUE(stored.has_value());
    EXPECT_TRUE(stored->state().report.has_value());  // 报告照样生成
    // 方案编排不受影响：照常可以编辑别的方案
    EXPECT_TRUE(authoring->createRecipe("别的方案").ok());
}

// A3 + A9，用“记下收到的全部内容”的假服务来验证：
// 默认策略下，发给大模型的内容里没有盲测集的任何用例，没有图像，也没有图像的路径
TEST_F(AssistantIntegration, LlmAdvisor_NothingForbiddenLeavesTheMachine) {
    ScriptedLlmService service({
        R"({"rationale": "a", "changes": [{"node": "bin", "param": "value", "value": 140}]})",
        R"({"rationale": "b", "changes": [{"node": "bin", "param": "value", "value": 195}]})",
        R"({"rationale": "c", "changes": [{"node": "blobs", "param": "minArea", "value": 30}]})",
    });
    int imagesLoaded = 0;
    llm::LlmAdvisor advisor = fakeLlmAdvisor(service, [&imagesLoaded](const std::string&, int) -> Result<llm::ChatImage> {
        ++imagesLoaded;
        return llm::ChatImage{"image/jpeg", "QUJD", ""};
    });
    AssistService assist(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    auto result = assist.run(makeRequest(0.6));  // 默认 dataSharing = no-images
    ASSERT_TRUE(result.ok()) << result.error().message;
    ASSERT_EQ(service.bodies.size(), 3u);
    for (const auto& body : service.bodies) {
        EXPECT_EQ(body.find("blind_"), std::string::npos);     // 盲测集的用例 ID
        EXPECT_EQ(body.find(".png"), std::string::npos);       // 图像的路径
        EXPECT_EQ(body.find("image_url"), std::string::npos);  // 图像本身
        EXPECT_EQ(body.find("KEY"), std::string::npos);        // 密钥不在请求体里
        EXPECT_NE(body.find("ok0"), std::string::npos);        // 调参集的用例是有的
        EXPECT_NE(body.find("p99"), std::string::npos);        // 统计信息是有的：这是大模型的“眼睛”
    }
    EXPECT_EQ(imagesLoaded, 0);
    // 盲测的结果（4 个漏检）只在报告里，从来没有发给大模型
    EXPECT_TRUE(result.value().overfitSuspected);
}

// 策略允许时才外发图像，而且只有调参集的
TEST_F(AssistantIntegration, LlmAdvisor_ThumbnailPolicySendsOnlyTuningImages) {
    ScriptedLlmService service({R"({"rationale": "够了", "changes": [], "finish": true})"});
    std::vector<std::pair<std::string, int>> loaded;
    llm::LlmAdvisor advisor = fakeLlmAdvisor(service, [&loaded](const std::string& ref, int maxSide) -> Result<llm::ChatImage> {
        loaded.emplace_back(ref, maxSide);
        return llm::ChatImage{"image/jpeg", "QUJD", ""};
    });
    AssistService assist(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    auto request = makeRequest(1.0);
    request.dataSharing = "thumbnails";
    ASSERT_TRUE(assist.run(request).ok());
    ASSERT_FALSE(loaded.empty());
    EXPECT_LE(loaded.size(), 6u);  // 一轮最多附 6 幅
    for (const auto& [ref, maxSide] : loaded) {
        EXPECT_EQ(ref.find("blind_"), std::string::npos) << ref;
        EXPECT_EQ(maxSide, 256);  // 只发缩略图
    }
    EXPECT_NE(service.bodies[0].find("image_url"), std::string::npos);
}

// ---------------- 加固阶段（真实算子）----------------
// 工作流声明的稳健性检查：亮度 ×0.8、×1.1，噪声 σ=8。

// 脚本顾问先调到达标（阈值 195：亮度 ×0.8 时亮斑只有 188，全部漏检），再把阈值降到 175（两头都留有余地）
TEST_F(AssistantIntegration, Hardening_KeepsTheMoreRobustCandidateAfterCriteriaMet) {
    std::vector<Proposal> script = goodScript();                              // 达标：阈值 195，最小面积 30
    script.push_back(setParam("bin", "value", 210, "试试更高"));              // 更不稳 → 回退
    script.push_back(setParam("bin", "value", 175, "离背景和亮斑都远一些"));  // 更稳 → 保留
    ScriptedAdvisor advisor("scripted:test", script);
    auto request = makeRequest(1.0);
    request.hardeningRounds = 4;
    auto result = run(advisor, request);
    ASSERT_TRUE(result.ok()) << result.error().message;
    const SessionView& v = result.value();

    EXPECT_EQ(v.endReason, "criteria-met");
    EXPECT_TRUE(v.tuningMeetsCriteria);
    EXPECT_EQ(v.hardeningRounds, 2);
    EXPECT_EQ(draftParam(v.candidateRecipeId, "vision.threshold", "value"), 175);  // 候选方案停在更稳的参数上

    auto stored = sessions->find(v.sessionId);
    ASSERT_TRUE(stored.has_value());
    const AssistReport& r = *stored->state().report;
    EXPECT_EQ(r.valuesWhenFirstMet.at("bin.value"), 195);
    EXPECT_EQ(r.finalValues.at("bin.value"), 175);
    const auto failed = [](const std::vector<RobustnessResult>& rs) {
        return std::count_if(rs.begin(), rs.end(), [](const RobustnessResult& x) { return !x.meetsCriteria; });
    };
    EXPECT_GT(failed(r.robustnessWhenFirstMet), 0);
    EXPECT_EQ(failed(r.robustness), 0);  // 加固之后声明的扰动全部通过
    EXPECT_NE(v.reportMarkdown.find("加固之后"), std::string::npos);

    // 顾问在加固阶段被告知了阶段和稳健性检查的结果，以及判错的用例在扰动之后的表现
    const auto& hardeningView = advisor.seen()[3];
    EXPECT_TRUE(hardeningView.hardening);
    EXPECT_FALSE(hardeningView.robustness.empty());
    ASSERT_FALSE(hardeningView.probeObservations.empty());
    for (const auto& p : hardeningView.probeObservations) {
        EXPECT_EQ(p.observation.caseId.find("blind_"), std::string::npos);  // A3
    }
    EXPECT_FALSE(advisor.seen()[0].hardening);
}

// 搜索顾问不知道什么是“加固”，但只要它继续试，助手就会留下更稳的那个
TEST_F(AssistantIntegration, Hardening_SearchAdvisorEndsUpMoreRobust) {
    assist_adapters::SearchAdvisor advisor;
    AssistService service(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    auto request = makeRequest(1.0);
    request.maxRounds = 40;
    request.hardeningRounds = 12;
    auto result = service.run(request);
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_TRUE(result.value().tuningMeetsCriteria);
    ASSERT_TRUE(result.value().blindMeetsCriteria.has_value());
    EXPECT_TRUE(*result.value().blindMeetsCriteria);

    auto stored = sessions->find(result.value().sessionId);
    ASSERT_TRUE(stored.has_value());
    const AssistReport& r = *stored->state().report;
    // 加固不会让方案变得更不稳：最终的不比刚达标时的差
    EXPECT_FALSE(moreRobust(r.robustnessWhenFirstMet, r.robustness));
    std::cout << "[测量] 搜索顾问：共 " << r.rounds << " 轮，其中加固 " << r.hardeningRounds << " 轮\n";
}

// 加固阶段的内容也到了大模型那里，而且同样不含盲测集
TEST_F(AssistantIntegration, Hardening_LlmAdvisorIsToldThePhaseAndWhatFails) {
    ScriptedLlmService service({
        R"({"rationale": "a", "changes": [{"node": "bin", "param": "value", "value": 195}, {"node": "blobs", "param": "minArea", "value": 30}]})",
        R"({"rationale": "b", "changes": [{"node": "bin", "param": "value", "value": 175}]})",
    });
    llm::LlmAdvisor advisor = fakeLlmAdvisor(service);
    AssistService assist(*workflows, *sessions, advisor, *editor, *runner, *goldenCases, bus);
    auto request = makeRequest(1.0);
    request.hardeningRounds = 4;
    auto result = assist.run(request);
    ASSERT_TRUE(result.ok()) << result.error().message;
    ASSERT_EQ(service.bodies.size(), 2u);
    EXPECT_NE(service.bodies[0].find("tuning"), std::string::npos);
    EXPECT_NE(service.bodies[1].find("hardening"), std::string::npos);
    EXPECT_NE(service.bodies[1].find("robustness_wrong_cases"), std::string::npos);
    EXPECT_EQ(service.bodies[1].find("blind_"), std::string::npos);
    EXPECT_EQ(service.bodies[1].find(".png"), std::string::npos);
    EXPECT_EQ(result.value().endReason, "criteria-met");
}

// ---------------- 示例工作流 ----------------

// samples/workflows/ 下的每一个工作流都必须能导入、能搭出候选方案、能在图像上跑起来。
// （工作流里写错了算子名、端口名、参数名或者参数越界，会在这里暴露。）
TEST_F(AssistantIntegration, EverySampleWorkflowBuildsAndRuns) {
    int count = 0;
    for (const auto& entry : fs::directory_iterator(fs::path(VISION_SOURCE_DIR) / "samples" / "workflows")) {
        if (entry.path().extension() != ".json") continue;
        ++count;
        auto id = workflows->importFile(entry.path());
        ASSERT_TRUE(id.ok()) << entry.path() << "：" << id.error().message;

        ScriptedAdvisor advisor("scripted:none", {});  // 不提任何修改：只检查工作流本身
        auto request = makeRequest(1.0);
        request.workflowId = id.value();
        // 工作流有模板槽的话，随便从一个调参用例上框一块给它：这里只检查工作流本身能不能搭起来、跑起来
        const auto workflow = workflows->find(id.value());  // 先存下来：直接遍历临时对象的成员会访问到已经销毁的内存
        ASSERT_TRUE(workflow.has_value());
        for (const auto& slot : workflow->templateSlots()) request.templates.push_back({slot.name, "ok0", 20, 20, 60, 40});
        auto result = run(advisor, request);
        ASSERT_TRUE(result.ok()) << id.value() << "：" << result.error().message;

        auto stored = sessions->find(result.value().sessionId);
        ASSERT_TRUE(stored.has_value());
        EXPECT_EQ(stored->state().baseline->errors, 0) << id.value() << "：有用例运行出错";
        // 候选方案是一份合法的、可以发布的方案（有判定依据、没有悬空的输入）
        EXPECT_TRUE(authoring->publish(result.value().candidateRecipeId).ok()) << id.value();
    }
    EXPECT_GE(count, 4);
}

// 宽度测量：卡尺 + 范围判定走完整个引擎，判定依据是操作员看得懂的话
TEST_F(AssistantIntegration, WidthMeasurementRecipeEndToEnd) {
    const auto recipe = authoring->createRecipe("宽度").value();
    const auto input = authoring->addNode(recipe, "vision.image_input").value();
    const auto toGray = authoring->addNode(recipe, "vision.to_gray").value();
    const auto caliper = authoring->addNode(recipe, "vision.caliper_width").value();
    const auto judge = authoring->addNode(recipe, "vision.judge_range").value();
    ASSERT_TRUE(authoring->connect(recipe, input + ".image", toGray + ".image").ok());
    ASSERT_TRUE(authoring->connect(recipe, toGray + ".image", caliper + ".image").ok());
    ASSERT_TRUE(authoring->connect(recipe, caliper + ".width", judge + ".value").ok());
    ASSERT_TRUE(authoring->setParam(recipe, judge, "min", 95.0).ok());
    ASSERT_TRUE(authoring->setParam(recipe, judge, "max", 105.0).ok());
    ASSERT_TRUE(authoring->setParam(recipe, judge, "label", std::string("宽度")).ok());
    // 连线的类型检查仍然有效：数值不能接到要图像的端口上
    EXPECT_FALSE(authoring->connect(recipe, caliper + ".width", toGray + ".image").ok());
    const auto version = authoring->publish(recipe);
    ASSERT_TRUE(version.ok()) << version.error().message;
    const auto doc = authoring->getVersionDocument(version.value()).value();

    const auto part = [](int width) {
        cv::Mat m(120, 300, CV_8UC3, cv::Scalar(150, 150, 150));
        cv::rectangle(m, cv::Rect(80, 20, width, 80), cv::Scalar(70, 70, 70), cv::FILLED);
        return imaging::fromMat(m).value();
    };
    const auto good = execution->inspect(doc, part(100)).value();
    EXPECT_EQ(good.verdict, "OK");
    ASSERT_EQ(good.reasons.size(), 1u);
    EXPECT_EQ(good.reasons[0].text, "宽度 100，允许 95 ~ 105");
    EXPECT_EQ(execution->inspect(doc, part(110)).value().verdict, "NG");
    // 画面里没有零件：量不到宽度，判不合格而不是出错
    cv::Mat empty(120, 300, CV_8UC3, cv::Scalar(150, 150, 150));
    const auto none = execution->inspect(doc, imaging::fromMat(empty).value()).value();
    EXPECT_EQ(none.verdict, "NG");
}

// ---------------- 带匹配模板的工作流（docs/ddd/07-recipe-assistant.md §8.5）----------------

namespace {

// 浅色背景上一块深色零件（宽 width），上面有一个亮的圆和一个小方块（用来定位）。
// 整体平移 (dx, dy)、顺时针转 angle 度。零件在标准位置时左边缘在 x = 60。
cv::Mat locatePart(double width, double dx, double dy, double angle) {
    cv::Mat m(200, 280, CV_8UC3, cv::Scalar(150, 150, 150));
    cv::rectangle(m, cv::Rect(60, 50, static_cast<int>(width), 100), cv::Scalar(70, 70, 70), cv::FILLED);
    cv::circle(m, cv::Point(95, 85), 10, cv::Scalar(225, 225, 225), cv::FILLED, cv::LINE_AA);
    cv::rectangle(m, cv::Rect(115, 98, 14, 9), cv::Scalar(200, 200, 200), cv::FILLED);
    const double radians = angle * CV_PI / 180.0, c = std::cos(radians), sn = std::sin(radians);
    const cv::Mat move = (cv::Mat_<double>(2, 3) << c, -sn, 140 - (c * 140 - sn * 100) + dx, sn, c, 100 - (sn * 140 + c * 100) + dy);
    cv::Mat out;
    cv::warpAffine(m, out, move, m.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(150, 150, 150));
    return out;
}

const char* kLocateWorkflow = R"({
  "id": "locate-width", "name": "定位后测宽", "description": "模板匹配 → 对齐 → 卡尺 → 判定",
  "nodes": [
    {"key": "input", "operator": "vision.image_input"},
    {"key": "gray", "operator": "vision.to_gray"},
    {"key": "match", "operator": "vision.template_match", "params": {"angleRange": 12, "minScore": 0.5}},
    {"key": "located", "operator": "vision.judge_range", "params": {"min": 0.6, "max": 1, "label": "定位得分"}},
    {"key": "align", "operator": "vision.align_to_pose", "params": {"border": "replicate"}},
    {"key": "caliper", "operator": "vision.caliper_width"},
    {"key": "judge", "operator": "vision.judge_range", "params": {"min": 157, "max": 163, "label": "宽度"}}
  ],
  "edges": [["input.image", "gray.image"], ["gray.image", "match.image"], ["match.score", "located.value"],
            ["gray.image", "align.image"], ["match.pose", "align.pose"], ["align.image", "caliper.image"],
            ["caliper.width", "judge.value"]],
  "templates": [{"name": "locator", "node": "match", "param": "template", "from": "gray.image", "hint": "框圆和小方块"}],
  "tunables": [{"node": "caliper", "param": "edgeThreshold", "min": 0.5, "max": 100, "initial": 90}],
  "robustness": [{"kind": "brightness", "levels": [0.8]}]
})";

}  // namespace

class AssistantLocating : public AssistantIntegration {
protected:
    void SetUp() override {
        AssistantIntegration::SetUp();
        const fs::path file = root / "locate-width.json";
        std::ofstream(file, std::ios::binary) << kLocateWorkflow;
        ASSERT_TRUE(workflows->importFile(file).ok());
    }

    // 8 个调参用例 + 4 个盲测用例。第一个调参用例（ok0）摆在标准位置，用来框模板；其余的位置、角度各不相同。
    // 合格的宽 160，不合格的宽 150 或 170。
    AssistRequest locateRequest() {
        release::GoldenSetDefinition def;
        def.name = "定位后测宽";
        AssistRequest request;
        int n = 0;
        const auto add = [&](const std::string& id, double width, bool blind) {
            const bool reference = id == "ok0";
            const double dx = reference ? 0 : ((n * 7) % 31) - 15, dy = reference ? 0 : ((n * 11) % 25) - 12;
            const double angle = reference ? 0 : ((n * 5) % 15) - 7;
            ++n;
            const std::string file = pathToUtf8(root / "images" / (id + ".png"));
            EXPECT_TRUE(imaging::saveImage(file, imaging::fromMat(locatePart(width, dx, dy, angle)).value()).ok());
            def.cases.push_back({id, file, width != 160, {}});
            if (blind) request.blindCaseIds.push_back(id);
        };
        for (int i = 0; i < 4; ++i) {
            add("ok" + std::to_string(i), 160, false);
            add("ng" + std::to_string(i), i % 2 == 0 ? 150 : 170, false);
        }
        for (int i = 0; i < 2; ++i) {
            add("blind_ok" + std::to_string(i), 160, true);
            add("blind_ng" + std::to_string(i), i % 2 == 0 ? 170 : 150, true);
        }
        request.goldenSetId = golden->createGoldenSet(def).value();
        request.goal = "零件位置不固定，测量宽度";
        request.workflowId = "locate-width";
        request.hardeningRounds = 0;
        request.templates = {{"locator", "ok0", 78, 68, 60, 46}};  // 圆和小方块那一带
        return request;
    }
};

// 助手按任务书给的模板搭出带定位的候选方案，并把它调到达标：零件的位置和角度各不相同，宽度照样量得准
TEST_F(AssistantLocating, BuildsCandidateWithTheTemplateFromTheBriefAndTunesIt) {
    ScriptedAdvisor advisor("scripted:test", {setParam("caliper", "edgeThreshold", 20, "90 太高，找不到边缘")});
    auto result = run(advisor, locateRequest());
    ASSERT_TRUE(result.ok()) << result.error().message;
    const SessionView& v = result.value();
    EXPECT_EQ(v.endReason, "criteria-met");
    EXPECT_TRUE(v.tuningMeetsCriteria);
    ASSERT_TRUE(v.blindMeetsCriteria.has_value());
    EXPECT_TRUE(*v.blindMeetsCriteria);

    // 候选方案里有任务书给的那个模板，定位节点用的就是它
    const auto draft = authoring->getDraft(v.candidateRecipeId).value();
    ASSERT_EQ(draft.templates.size(), 1u);
    EXPECT_EQ(draft.templates[0].name, "locator");
    EXPECT_EQ(draft.templates[0].width, 60);
    EXPECT_EQ(draft.templates[0].referenceX, 78);
    EXPECT_TRUE(contentStore->contains(draft.templates[0].sha256));
    bool used = false;
    for (const auto& n : draft.nodes) {
        if (n.operatorId == "vision.template_match") used = std::get<std::string>(n.params.at("template")) == draft.templates[0].id;
    }
    EXPECT_TRUE(used);
    // 模板是从“灰度”节点的输出上框的（工作流里写的 from），所以是单通道的图像：定位节点看到的就是这种图像
    const auto doc = authoring->getDraftDocument(v.candidateRecipeId).value();
    const auto templ = templateContent->load(doc.templates[0]);
    ASSERT_TRUE(templ.ok());
    EXPECT_EQ(templ.value().format(), Image::Format::Gray8);

    // 任务书里的模板进了会话记录
    auto stored = sessions->find(v.sessionId);
    ASSERT_TRUE(stored.has_value());
    ASSERT_EQ(stored->state().brief.templates.size(), 1u);
    EXPECT_EQ(stored->state().brief.templates[0].caseId, "ok0");
    EXPECT_EQ(stored->workflow().templateSlots().size(), 1u);
    // 采纳之后可以发布：模板参数已经设置（P6）
    EXPECT_TRUE(authoring->publish(v.candidateRecipeId).ok());
}

// A12：模板只能取自调参集。用盲测集的图像做模板，等于把盲测集的内容放进了方案
TEST_F(AssistantLocating, A12_TemplateMustComeFromATuningCase) {
    ScriptedAdvisor advisor("scripted:test", {});
    auto request = locateRequest();
    request.templates = {{"locator", "blind_ok0", 78, 68, 60, 46}};
    const auto result = run(advisor, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, "assistant.template_from_blind");
    EXPECT_TRUE(sessions->list().empty());  // 没有开始会话
    EXPECT_TRUE(advisor.seen().empty());    // 顾问没有被问过
}

TEST_F(AssistantLocating, A12_EverySlotNeedsExactlyOneTemplate) {
    ScriptedAdvisor advisor("scripted:test", {});
    auto request = locateRequest();
    request.templates.clear();
    auto result = run(advisor, request);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, "assistant.missing_template");
    EXPECT_NE(result.error().message.find("框圆和小方块"), std::string::npos);  // 把工作流里“应该框什么”的说明带给用户

    request = locateRequest();
    request.templates.push_back({"nope", "ok0", 10, 10, 20, 20});
    EXPECT_EQ(run(advisor, request).error().code, "assistant.unknown_template_slot");

    request = locateRequest();
    request.templates.push_back({"locator", "ok1", 10, 10, 20, 20});
    EXPECT_EQ(run(advisor, request).error().code, "assistant.duplicate_template");

    request = locateRequest();
    request.templates = {{"locator", "no-such-case", 78, 68, 60, 46}};
    EXPECT_EQ(run(advisor, request).error().code, "assistant.template_case");

    // 没有模板槽的工作流不接受模板
    auto plain = makeRequest(1.0);
    plain.templates = {{"locator", "ok0", 20, 20, 60, 40}};
    EXPECT_EQ(run(advisor, plain).error().code, "assistant.unknown_template_slot");
}

// 框出了图像的范围：方案编排拒绝，助手如实报告，不留下半截的会话
TEST_F(AssistantLocating, BadTemplateBoxIsReportedClearly) {
    ScriptedAdvisor advisor("scripted:test", {});
    auto request = locateRequest();
    request.templates = {{"locator", "ok0", 250, 68, 60, 46}};  // 右边伸出了图像
    const auto result = run(advisor, request);
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.error().message.find("locator"), std::string::npos);
    EXPECT_TRUE(sessions->list().empty());
}

// 顾问仍然只能调数值参数：想改模板参数会被规则拒绝（模板由人定）
TEST_F(AssistantLocating, AdvisorCannotChangeTheTemplate) {
    Proposal swap;
    swap.changes.push_back({ProposedChange::Kind::SetParam, "match", "template", 2});
    ScriptedAdvisor advisor("scripted:test", {swap, setParam("caliper", "edgeThreshold", 20)});
    auto result = run(advisor, locateRequest());
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().rejected, 1);
    EXPECT_EQ(result.value().endReason, "criteria-met");
}
