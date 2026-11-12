// 集成测试：真实算子 + OpenCV + JSON 文件仓储 + 事件总线，走完整流程：
// 搭方案 → 发布 → 取发布语言 → 检测 → 判定和判定依据 → 领域事件。

#include <gtest/gtest.h>

#include <filesystem>
#include <opencv2/imgproc.hpp>

#include "vision/authoring/api.h"
#include "vision/catalog/api.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/execution/api.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/persistence/json_repositories.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

Image makeProduct(int spots) {
    cv::Mat m(240, 320, CV_8UC3, cv::Scalar(150, 150, 150));
    cv::rectangle(m, cv::Rect(60, 40, 200, 160), cv::Scalar(70, 70, 70), cv::FILLED);
    for (int i = 0; i < spots; ++i) cv::circle(m, cv::Point(90 + 50 * i, 100), 6, cv::Scalar(240, 240, 240), cv::FILLED);
    return imaging::fromMat(m).value();
}

class Platform : public ::testing::Test {
protected:
    fs::path dir = fs::temp_directory_path() / ("vision-it-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                                "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    eventbus::InMemoryEventBus bus;
    std::vector<std::string> events;
    persistence::JsonRecipeRepository recipes{dir};
    persistence::JsonRecipeVersionRepository versions{dir};
    authoring::AuthoringService authoring{recipes, versions, catalog::OperatorCatalog::global(), bus};
    execution::ExecutionService execution{catalog::OperatorCatalog::global(), bus};

    void SetUp() override {
        bus.subscribe("*", [this](const EventPtr& e) { events.push_back(e->name()); });
    }
    void TearDown() override { fs::remove_all(dir); }

    // 输入 → 灰度 → 阈值(200) → Blob(面积≥20) → 计数判定(上限 0)
    std::string buildRecipe() {
        auto rid = authoring.createRecipe("亮斑检测").value();
        auto in = authoring.addNode(rid, "vision.image_input").value();
        auto gray = authoring.addNode(rid, "vision.to_gray").value();
        auto thr = authoring.addNode(rid, "vision.threshold").value();
        auto blob = authoring.addNode(rid, "vision.blob").value();
        auto judge = authoring.addNode(rid, "vision.judge_count").value();
        EXPECT_TRUE(authoring.setParam(rid, thr, "value", 200.0).ok());
        EXPECT_TRUE(authoring.setParam(rid, blob, "minArea", 20.0).ok());
        EXPECT_TRUE(authoring.setParam(rid, judge, "label", std::string("亮斑")).ok());
        EXPECT_TRUE(authoring.connect(rid, in + ".image", gray + ".image").ok());
        EXPECT_TRUE(authoring.connect(rid, gray + ".image", thr + ".image").ok());
        EXPECT_TRUE(authoring.connect(rid, thr + ".region", blob + ".region").ok());
        EXPECT_TRUE(authoring.connect(rid, blob + ".blobs", judge + ".blobs").ok());
        return rid;
    }
};

}  // namespace

TEST_F(Platform, AllOperatorsRegistered) {
    EXPECT_TRUE(catalog::OperatorCatalog::global().registrationErrors().empty());
    for (const char* id : {"vision.image_input", "vision.to_gray", "vision.threshold", "vision.blob", "vision.judge_count"}) {
        EXPECT_TRUE(catalog::OperatorCatalog::global().find(id).has_value()) << id;
    }
}

TEST_F(Platform, EndToEndInspection) {
    auto rid = buildRecipe();
    auto vid = authoring.publish(rid);
    ASSERT_TRUE(vid.ok()) << vid.error().message;
    auto doc = authoring.getVersionDocument(vid.value());
    ASSERT_TRUE(doc.ok());

    auto ok = execution.inspect(doc.value(), makeProduct(0));
    ASSERT_TRUE(ok.ok()) << ok.error().message;
    EXPECT_EQ(ok.value().verdict, "OK");

    auto ng = execution.inspect(doc.value(), makeProduct(2));
    ASSERT_TRUE(ng.ok());
    EXPECT_EQ(ng.value().verdict, "NG");
    ASSERT_EQ(ng.value().reasons.size(), 1u);
    EXPECT_EQ(ng.value().reasons[0].text, "亮斑 2 处，上限 0 处");
    EXPECT_EQ(ng.value().nodes.size(), 5u);

    EXPECT_EQ(events, (std::vector<std::string>{"RecipeCreated", "RecipeVersionPublished", "InspectionCompleted",
                                                "InspectionCompleted"}));
}

TEST_F(Platform, RoiLimitsThresholdArea) {
    auto rid = buildRecipe();
    auto draft = authoring.getDraft(rid).value();
    // 只检查左半边：右侧的第 2 个亮斑（x=140）也在左半边内，第 3 个（x=190）不在
    ASSERT_TRUE(authoring.setRoi(rid, draft.nodes[2].id, Roi::rect(0, 0, 165, 240).value()).ok());
    auto doc = authoring.getVersionDocument(authoring.publish(rid).value()).value();
    auto r = execution.inspect(doc, makeProduct(3));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().reasons[0].text, "亮斑 2 处，上限 0 处");
}

TEST_F(Platform, DebugRunShowsIntermediateResultsWithoutCompletedEvent) {
    auto rid = buildRecipe();
    events.clear();
    auto doc = authoring.getDraftDocument(rid);
    ASSERT_TRUE(doc.ok());
    auto r = execution.debugRun(doc.value(), makeProduct(1));
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().report.verdict, "NG");
    EXPECT_EQ(r.value().outputs.size(), 5u);
    EXPECT_TRUE(events.empty()) << "调试运行不应发布 InspectionCompleted";
}

TEST_F(Platform, OperatorFailureGivesErrorVerdict) {
    // 阈值算子要求灰度图：去掉灰度节点、直接把彩色图接到阈值上 → 阈值节点失败 → ERROR
    auto rid = authoring.createRecipe("故意出错").value();
    auto in = authoring.addNode(rid, "vision.image_input").value();
    auto thr = authoring.addNode(rid, "vision.threshold").value();
    auto blob = authoring.addNode(rid, "vision.blob").value();
    auto judge = authoring.addNode(rid, "vision.judge_count").value();
    ASSERT_TRUE(authoring.connect(rid, in + ".image", thr + ".image").ok());
    ASSERT_TRUE(authoring.connect(rid, thr + ".region", blob + ".region").ok());
    ASSERT_TRUE(authoring.connect(rid, blob + ".blobs", judge + ".blobs").ok());
    auto doc = authoring.getVersionDocument(authoring.publish(rid).value()).value();
    auto r = execution.inspect(doc, makeProduct(0));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().verdict, "ERROR");
    EXPECT_EQ(r.value().nodes[1].status, "failed");
    EXPECT_EQ(r.value().nodes[2].status, "skipped");
}

TEST_F(Platform, PersistenceRoundTrip) {
    auto rid = buildRecipe();
    auto vid = authoring.publish(rid).value();

    // 新建一套服务读取同一个目录：从文件重建聚合
    persistence::JsonRecipeRepository recipes2{dir};
    persistence::JsonRecipeVersionRepository versions2{dir};
    authoring::AuthoringService authoring2{recipes2, versions2, catalog::OperatorCatalog::global(), bus};
    auto doc1 = authoring.getVersionDocument(vid).value();
    auto doc2 = authoring2.getVersionDocument(vid).value();
    EXPECT_EQ(persistence::toJson(doc1), persistence::toJson(doc2));

    // 发布语言 JSON 往返
    auto parsed = persistence::recipeDocumentFromJson(persistence::toJson(doc1));
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(persistence::toJson(parsed.value()), persistence::toJson(doc1));

    // 草稿重建后可以继续编辑，节点编号接着往下
    EXPECT_EQ(authoring2.addNode(rid, "vision.to_gray").value(), "n6");
    // n6 的输入没连，发布规则 P1 拒绝发布
    EXPECT_EQ(authoring2.publish(rid).error().code, "authoring.publish_rejected");
    ASSERT_TRUE(authoring2.removeNode(rid, "n6").ok());
    EXPECT_EQ(authoring2.publish(rid).value(), rid + "@v2");
}

TEST_F(Platform, RejectedCommandsDoNotChangeState) {
    auto rid = buildRecipe();
    auto before = authoring.getDraft(rid).value();
    EXPECT_FALSE(authoring.connect(rid, "n5.judgement", "n2.image").ok());  // 类型不兼容
    EXPECT_FALSE(authoring.connect(rid, "n4.blobs", "n5.blobs").ok());      // 输入已被占用
    EXPECT_FALSE(authoring.setParam(rid, "n3", "value", 999.0).ok());       // 超出范围
    auto after = authoring.getDraft(rid).value();
    EXPECT_EQ(before.edges.size(), after.edges.size());
    EXPECT_EQ(std::get<double>(after.nodes[2].params.at("value")), 200.0);
}
