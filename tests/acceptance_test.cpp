// 验收检查（REL-05，docs/ddd/15-acceptance-check.md）
//
//   1. 领域层：上线包上的验收记录，规则 A1～A4、L4
//   2. 整条路：真的图像、真的方案、整个平台——通过、没通过、不能部署、重启之后还在

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>

#include "platform.h"
#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/release/domain/golden_set.h"
#include "vision/release/domain/release.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

// 4 个合格的、2 个不合格的；验收标准由调用的一方给
release::GoldenSet goldenSet(release::AcceptanceCriteria criteria = {}, bool freeze = true) {
    using namespace release;
    auto set = GoldenSet::create("gs-1", "亮斑基准").value();
    for (const char* id : {"ok1", "ok2", "ok3", "ok4"}) EXPECT_TRUE(set.addCase({id, std::string(id) + ".png", Truth::Ok, {}}).ok());
    for (const char* id : {"ng1", "ng2"}) EXPECT_TRUE(set.addCase({id, std::string(id) + ".png", Truth::Ng, {}}).ok());
    EXPECT_TRUE(set.setCriteria(criteria).ok());
    if (freeze) EXPECT_TRUE(set.freeze().ok());
    return set;
}

// 全部判对，除了 wrong 里列出的用例（给出它们被判成了什么）
release::CaseEvaluation evaluate(const release::GoldenSet& set, const std::map<std::string, release::Observed>& wrong = {}) {
    using namespace release;
    std::vector<CaseResult> results;
    for (const auto& c : set.cases()) {
        const auto it = wrong.find(c.id);
        results.push_back({c.id, it != wrong.end() ? it->second : c.truth == Truth::Ng ? Observed::Ng : Observed::Ok, 2.0});
    }
    return evaluateCases(set, results).value();
}

}  // namespace

// ---------------- 1. 领域层 ----------------

TEST(ReleaseAcceptance, A3_PassOrFailComesFromTheCriteriaNotFromTheCaller) {
    using namespace release;
    auto r = Release::create("rel-1", "recipe@v1").value();
    (void)r.pullEvents();
    EXPECT_EQ(r.acceptanceState(), AcceptanceState::NotChecked);
    EXPECT_FALSE(r.deployable());  // L4：还没验收的不能部署

    const auto set = goldenSet();
    ASSERT_TRUE(r.recordAcceptance(set, evaluate(set), {}, 1000).ok());
    EXPECT_EQ(r.acceptanceState(), AcceptanceState::Passed);
    EXPECT_TRUE(r.deployable());
    EXPECT_EQ(r.acceptance()->total, 6);
    EXPECT_EQ(r.acceptance()->goldenSetName, "亮斑基准");
    EXPECT_EQ(r.acceptance()->checkedAtMs, 1000);
    auto events = r.pullEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_STREQ(events[0]->name(), "ReleaseAccepted");

    // 漏检一张，标准是一张都不许漏：没通过。L4：不能部署
    ASSERT_TRUE(r.recordAcceptance(set, evaluate(set, {{"ng1", Observed::Ok}}), {{"ng1", "ng1.png", true, "OK"}}, 2000).ok());
    EXPECT_EQ(r.acceptanceState(), AcceptanceState::Failed);
    EXPECT_FALSE(r.deployable());
    EXPECT_EQ(r.acceptance()->misses, 1);
    ASSERT_EQ(r.acceptance()->wrongCases.size(), 1u);
    events = r.pullEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_STREQ(events[0]->name(), "ReleaseAcceptanceFailed");
}

TEST(ReleaseAcceptance, TheSameResultPassesOrFailsDependingOnTheCriteria) {
    using namespace release;
    // 一张误报（4 张合格的里判错 1 张 = 25%）、一张出错
    const std::map<std::string, Observed> wrong = {{"ok1", Observed::Ng}, {"ok2", Observed::Error}};
    const auto verdict = [&](AcceptanceCriteria criteria) {
        auto r = Release::create("rel-1", "recipe@v1").value();
        const auto set = goldenSet(criteria);
        EXPECT_TRUE(r.recordAcceptance(set, evaluate(set, wrong), {}, 1).ok());
        // 标准抄在记录里：以后回头看，不用再去找基准图集
        EXPECT_DOUBLE_EQ(r.acceptance()->maxFalseAlarmRate, criteria.maxFalseAlarmRate);
        return r.acceptanceState() == AcceptanceState::Passed;
    };
    EXPECT_FALSE(verdict({0, 0.0, 0}));
    EXPECT_FALSE(verdict({0, 0.25, 0}));  // 误报率够了，但不许出错
    EXPECT_FALSE(verdict({0, 0.2, 1}));   // 许出错一张，但误报率超了
    EXPECT_TRUE(verdict({0, 0.25, 1}));
}

TEST(ReleaseAcceptance, A1_A2_WhatCannotBeRecorded) {
    using namespace release;
    auto r = Release::create("rel-1", "recipe@v1").value();
    const auto set = goldenSet();

    // A2：只跑了一部分用例
    std::vector<CaseResult> partial = {{"ok1", Observed::Ok, 1}, {"ng1", Observed::Ng, 1}};
    auto st = r.recordAcceptance(set, evaluateCases(set, partial).value(), {}, 1);
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, "release.acceptance_incomplete");
    // 还没冻结的基准图集
    const auto draft = goldenSet({}, false);
    EXPECT_EQ(r.recordAcceptance(draft, evaluate(draft), {}, 1).error().code, "release.golden_set_not_frozen");
    EXPECT_EQ(r.acceptanceState(), AcceptanceState::NotChecked);  // 被拒绝的不留痕迹

    // A1：撤回之后不再验收；原来的记录还在
    ASSERT_TRUE(r.recordAcceptance(set, evaluate(set), {}, 1).ok());
    ASSERT_TRUE(r.withdraw().ok());
    EXPECT_EQ(r.recordAcceptance(set, evaluate(set, {{"ng1", Observed::Ok}}), {}, 2).error().code, "release.withdrawn");
    EXPECT_EQ(r.acceptanceState(), AcceptanceState::Passed);
    EXPECT_FALSE(r.deployable());  // 验收通过也没用：撤回了（L6）
}

TEST(ReleaseAcceptance, A4_ALaterCheckReplacesTheEarlierOne) {
    using namespace release;
    auto r = Release::create("rel-1", "recipe@v1").value();
    const auto set = goldenSet();
    ASSERT_TRUE(r.recordAcceptance(set, evaluate(set, {{"ng1", Observed::Ok}}), {}, 1).ok());
    EXPECT_FALSE(r.deployable());
    ASSERT_TRUE(r.recordAcceptance(set, evaluate(set), {}, 2).ok());  // 例如换了一个用例标对了的基准图集
    EXPECT_TRUE(r.deployable());
    EXPECT_EQ(r.acceptance()->checkedAtMs, 2);
    EXPECT_TRUE(r.acceptance()->wrongCases.empty());
}

// ---------------- 2. 整条路 ----------------

namespace {

class Acceptance : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root = fs::temp_directory_path() / pathFromUtf8("vision-验收-" + std::string(info->name()));
        fs::remove_all(root);
        fs::create_directories(root / "ok");
        fs::create_directories(root / pathFromUtf8("不合格"));  // 中文目录名
        // 同样内容的图像是同一个样本，不能在一个图集里出现两次：三幅合格的背景亮度各不相同
        writeImage(root / "ok" / "1.png", 0, 80);
        writeImage(root / "ok" / "2.png", 0, 90);
        writeImage(root / "ok" / "3.JPG", 0, 100);
        std::ofstream(root / "ok" / "notes.txt") << "不是图像";
        writeImage(root / pathFromUtf8("不合格") / "a.png", 1);
        writeImage(root / pathFromUtf8("不合格") / "b.png", 3);
        open();
        // 输入 → 灰度 → 阈值 → Blob → 个数判定。严格的：一个亮斑都不许有；宽松的：最多 2 个
        auto& a = p->authoring;
        const auto recipe = a.createRecipe("亮斑").value();
        std::vector<std::string> n;
        for (const char* op : {"vision.image_input", "vision.to_gray", "vision.threshold", "vision.blob", "vision.judge_count"}) {
            n.push_back(a.addNode(recipe, op).value());
        }
        ASSERT_TRUE(a.connect(recipe, n[0] + ".image", n[1] + ".image").ok());
        ASSERT_TRUE(a.connect(recipe, n[1] + ".image", n[2] + ".image").ok());
        ASSERT_TRUE(a.connect(recipe, n[2] + ".region", n[3] + ".region").ok());
        ASSERT_TRUE(a.connect(recipe, n[3] + ".blobs", n[4] + ".blobs").ok());
        ASSERT_TRUE(a.setParam(recipe, n[2], "value", 200.0).ok());
        strict = p->releases.createRelease(a.publish(recipe).value()).value();
        ASSERT_TRUE(a.setParam(recipe, n[4], "maxCount", 2.0).ok());
        lenient = p->releases.createRelease(a.publish(recipe).value()).value();
    }
    void TearDown() override {
        p.reset();
        fs::remove_all(root);
    }
    void open() {
        p.reset();
        p = std::make_unique<platform::Platform>(root / pathFromUtf8("数据"));
    }
    static void writeImage(const fs::path& file, int spots, int background = 80) {
        cv::Mat m(120, 160, CV_8UC1, cv::Scalar(background));
        for (int i = 0; i < spots; ++i) cv::circle(m, {30 + 40 * i, 60}, 8, cv::Scalar(240), cv::FILLED);
        ASSERT_TRUE(imaging::saveImage(pathToUtf8(file), imaging::fromMat(m).value()).ok());
    }
    std::string golden(release::CriteriaView criteria = {}) {
        auto id = platform::createGoldenSetFromFolders(*p, "亮斑基准", pathToUtf8(root / "ok"), pathToUtf8(root / pathFromUtf8("不合格")), criteria);
        EXPECT_TRUE(id.ok()) << (id ? "" : id.error().message);
        return id ? id.value() : "";
    }

    fs::path root;
    std::unique_ptr<platform::Platform> p;
    std::string strict, lenient;
};

}  // namespace

TEST_F(Acceptance, GoldenSetFromTwoFolders) {
    const auto set = p->goldenSets.getGoldenSet(golden({1, 0.02, 0})).value();
    ASSERT_EQ(set.cases.size(), 5u);  // notes.txt 不算；.JPG 算
    EXPECT_EQ(set.cases[0].id, "ok/1.png");
    EXPECT_FALSE(set.cases[0].truthNg);
    EXPECT_EQ(set.cases[3].id, "ng/a.png");
    EXPECT_TRUE(set.cases[4].truthNg);
    EXPECT_TRUE(fs::path(pathFromUtf8(set.cases[3].imageRef)).is_absolute());  // 换个工作目录也找得到
    EXPECT_EQ(set.criteria.maxMisses, 1);
    EXPECT_DOUBLE_EQ(set.criteria.maxFalseAlarmRate, 0.02);

    // 只有一类也可以；目录不存在、一幅图像都没有：说清楚
    EXPECT_TRUE(platform::createGoldenSetFromFolders(*p, "只有合格的", pathToUtf8(root / "ok"), "", {}).ok());
    EXPECT_EQ(platform::createGoldenSetFromFolders(*p, "x", pathToUtf8(root / "nope"), "", {}).error().code, "golden.folder_not_found");
    fs::create_directories(root / "empty");
    EXPECT_EQ(platform::createGoldenSetFromFolders(*p, "x", pathToUtf8(root / "empty"), "", {}).error().code, "golden.no_images");
    EXPECT_FALSE(platform::createGoldenSetFromFolders(*p, "", pathToUtf8(root / "ok"), "", {}).ok());  // 没有名称
}

TEST_F(Acceptance, PassAndFailAgainstRealImages) {
    const std::string set = golden();
    // 还没验收
    EXPECT_EQ(p->releases.getManifest(strict).value().acceptance, "none");
    EXPECT_EQ(p->releases.getAcceptance(strict).error().code, "release.not_checked");

    std::vector<std::string> events;
    p->bus.subscribe("*", [&](const EventPtr& e) { events.push_back(e->name()); });

    // 严格的方案：5 个用例全对
    auto a = p->releases.checkAcceptance(strict, set);
    ASSERT_TRUE(a.ok()) << a.error().message;
    EXPECT_TRUE(a.value().passed);
    EXPECT_EQ(a.value().total, 5);
    EXPECT_EQ(a.value().truthOk, 3);
    EXPECT_EQ(a.value().truthNg, 2);
    EXPECT_EQ(a.value().misses + a.value().falseAlarms + a.value().errors, 0);
    EXPECT_GT(a.value().meanMs, 0);
    EXPECT_GT(a.value().checkedAtMs, 1700000000000);
    EXPECT_EQ(a.value().goldenSetName, "亮斑基准");
    EXPECT_EQ(p->releases.getManifest(strict).value().acceptance, "passed");

    // 宽松的方案（最多 2 个亮斑）：只有 1 个亮斑的那幅漏掉了
    a = p->releases.checkAcceptance(lenient, set);
    ASSERT_TRUE(a.ok());  // 检查做完了，结论是“不行”：这不是出错
    EXPECT_FALSE(a.value().passed);
    EXPECT_EQ(a.value().misses, 1);
    ASSERT_EQ(a.value().wrongCases.size(), 1u);
    EXPECT_EQ(a.value().wrongCases[0].caseId, "ng/a.png");
    EXPECT_EQ(a.value().wrongCases[0].kind, "miss");
    EXPECT_EQ(a.value().wrongCases[0].observed, "OK");
    EXPECT_TRUE(a.value().wrongCases[0].truthNg);
    EXPECT_EQ(p->releases.getManifest(lenient).value().acceptance, "failed");

    // 事件：一个通过、一个没通过
    EXPECT_EQ(std::count(events.begin(), events.end(), "ReleaseAccepted"), 1);
    EXPECT_EQ(std::count(events.begin(), events.end(), "ReleaseAcceptanceFailed"), 1);

    // 同一个方案，标准放宽到“可以漏 1 张”：通过。以最近的一次为准（A4）
    a = p->releases.checkAcceptance(lenient, golden({1, 0, 0}));
    ASSERT_TRUE(a.ok());
    EXPECT_TRUE(a.value().passed);
    EXPECT_EQ(a.value().misses, 1);
    EXPECT_EQ(a.value().maxMisses, 1);
    EXPECT_EQ(a.value().wrongCases.size(), 1u);  // 通过了，判错的那张照样列出来
}

TEST_F(Acceptance, L4_OnlyAnAcceptedReleaseCanBeDeployed) {
    const std::string set = golden();
    const std::string station = p->stations.createStation("一号").value();
    // 没验收的：不能部署，原因说的是“还没验收”
    auto st = p->stations.deploy(station, strict);
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, "station.release_not_accepted");
    EXPECT_NE(st.error().message.find("还没有做过验收检查"), std::string::npos);
    EXPECT_TRUE(p->stations.getStation(station).value().pendingRelease.empty());  // 被拒绝的不留痕迹
    // 没通过的：不能部署
    ASSERT_FALSE(p->releases.checkAcceptance(lenient, set).value().passed);
    st = p->stations.deploy(station, lenient);
    ASSERT_FALSE(st.ok());
    EXPECT_NE(st.error().message.find("没有通过"), std::string::npos);
    // 通过的：可以
    ASSERT_TRUE(p->releases.checkAcceptance(strict, set).value().passed);
    EXPECT_TRUE(p->stations.deploy(station, strict).ok());
    ASSERT_TRUE(p->stations.waitForLoading(station).ok());
    // 验收检查不算产线检测：不留检测记录
    EXPECT_EQ(p->quality.summary(station).total, 0);
}

// 调试用的口子：允许部署“还没验收”的；“验收没通过”的仍然不行
TEST_F(Acceptance, TheDebugSwitchLetsUncheckedReleasesThroughButNeverFailedOnes) {
    const std::string station = p->stations.createStation("一号").value();
    p->stations.allowUnacceptedReleases(true);
    EXPECT_TRUE(p->stations.deploy(station, lenient).ok());
    ASSERT_TRUE(p->stations.waitForLoading(station).ok());
    ASSERT_FALSE(p->releases.checkAcceptance(lenient, golden()).value().passed);
    const std::string other = p->stations.createStation("二号").value();
    EXPECT_EQ(p->stations.deploy(other, lenient).error().code, "station.release_not_accepted");
    p->stations.allowUnacceptedReleases(false);  // 关上之后回到规则
    EXPECT_FALSE(p->stations.deploy(other, strict).ok());
}

TEST_F(Acceptance, TheRecordSurvivesARestartAndOldReleaseFilesStillLoad) {
    const std::string set = golden();
    ASSERT_FALSE(p->releases.checkAcceptance(lenient, set).value().passed);
    // 以前存下的上线包文件里没有验收这一项：把它从 strict 的文件里去掉，模拟旧文件
    const fs::path file = root / pathFromUtf8("数据") / "releases" / (strict + ".json");
    {
        std::ifstream in(file);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("acceptance"), std::string::npos);
    }
    open();  // 程序重启了

    const auto a = p->releases.getAcceptance(lenient);
    ASSERT_TRUE(a.ok());
    EXPECT_FALSE(a.value().passed);
    EXPECT_EQ(a.value().goldenSetId, set);
    EXPECT_EQ(a.value().total, 5);
    EXPECT_EQ(a.value().misses, 1);
    ASSERT_EQ(a.value().wrongCases.size(), 1u);
    EXPECT_EQ(a.value().wrongCases[0].caseId, "ng/a.png");
    EXPECT_EQ(a.value().wrongCases[0].kind, "miss");
    const std::string station = p->stations.createStation("一号").value();
    EXPECT_EQ(p->stations.deploy(station, lenient).error().code, "station.release_not_accepted");
    // 旧的上线包文件：读得出来，状态是“没验收”，所以也不能部署
    EXPECT_EQ(p->releases.getManifest(strict).value().acceptance, "none");
    EXPECT_FALSE(p->stations.deploy(station, strict).ok());
}

TEST_F(Acceptance, ACheckThatCannotBeFinishedLeavesNoRecord) {
    const std::string set = golden();
    EXPECT_EQ(p->releases.checkAcceptance("nobody", set).error().code, "release.not_found");
    EXPECT_EQ(p->releases.checkAcceptance(strict, "nobody").error().code, "golden.not_found");

    // 基准图集里的一幅图像后来被删掉了：检查做不下去，说出是哪一个用例，不留记录
    fs::remove(root / "ok" / "2.png");
    const auto a = p->releases.checkAcceptance(strict, set);
    ASSERT_FALSE(a.ok());
    EXPECT_NE(a.error().message.find("ok/2.png"), std::string::npos) << a.error().message;
    EXPECT_EQ(p->releases.getManifest(strict).value().acceptance, "none");

    // 撤回了的上线包不再验收（A1）
    ASSERT_TRUE(p->releases.withdraw(lenient).ok());
    EXPECT_EQ(p->releases.checkAcceptance(lenient, set).error().code, "release.withdrawn");
}

// 检测本身出错（不是图像读不出来）：这个用例算 ERROR，检查照常做完
TEST_F(Acceptance, AnOperatorFailureCountsAsAnErrorCase) {
    // 阈值算子要求灰度图。这个方案没有“转灰度”的节点：灰度的图像照常检测，彩色的那一幅会让阈值节点失败
    auto& a = p->authoring;
    const auto recipe = a.createRecipe("没有转灰度").value();
    const auto in = a.addNode(recipe, "vision.image_input").value();
    const auto thr = a.addNode(recipe, "vision.threshold").value();
    const auto blob = a.addNode(recipe, "vision.blob").value();
    const auto judge = a.addNode(recipe, "vision.judge_count").value();
    ASSERT_TRUE(a.connect(recipe, in + ".image", thr + ".image").ok());
    ASSERT_TRUE(a.connect(recipe, thr + ".region", blob + ".region").ok());
    ASSERT_TRUE(a.connect(recipe, blob + ".blobs", judge + ".blobs").ok());
    ASSERT_TRUE(a.setParam(recipe, thr, "value", 200.0).ok());
    const std::string release = p->releases.createRelease(a.publish(recipe).value()).value();
    ASSERT_TRUE(imaging::saveImage(pathToUtf8(root / "ok" / "4-color.png"),
                                   imaging::fromMat(cv::Mat(120, 160, CV_8UC3, cv::Scalar(80, 80, 80))).value()).ok());

    auto checked = p->releases.checkAcceptance(release, golden({0, 0, 0}));
    ASSERT_TRUE(checked.ok()) << checked.error().message;
    EXPECT_EQ(checked.value().total, 6);
    EXPECT_EQ(checked.value().errors, 1);
    EXPECT_FALSE(checked.value().passed);
    ASSERT_EQ(checked.value().wrongCases.size(), 1u);
    EXPECT_EQ(checked.value().wrongCases[0].caseId, "ok/4-color.png");
    EXPECT_EQ(checked.value().wrongCases[0].kind, "error");
    EXPECT_EQ(checked.value().wrongCases[0].observed, "ERROR");
    // 标准里允许出错 1 张：通过
    checked = p->releases.checkAcceptance(release, golden({0, 0, 1}));
    ASSERT_TRUE(checked.ok());
    EXPECT_TRUE(checked.value().passed);
}

// 产线运行平台上没有基准图集：说清楚，而不是崩溃
TEST(AcceptanceOnTheLine, IsRefusedWithAReason) {
    const fs::path dir = fs::temp_directory_path() / "vision-acceptance-line";
    fs::remove_all(dir);
    {
        platform::LinePlatform line(dir);
        const auto a = line.releases.checkAcceptance("any", "any");
        ASSERT_FALSE(a.ok());
        EXPECT_EQ(a.error().code, "release.no_acceptance");
    }
    fs::remove_all(dir);
}
