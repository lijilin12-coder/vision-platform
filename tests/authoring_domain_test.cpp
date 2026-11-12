// 方案编排领域层测试：不变量 R1～R6、发布规则 P1 / P5、方案版本。
// 只链接 authoring_domain（不链接 OpenCV，不需要图像），毫秒级跑完。

#include <gtest/gtest.h>

#include "support/test_signatures.h"
#include "vision/authoring/domain/events.h"
#include "vision/authoring/domain/recipe.h"

using namespace vision;
using namespace vision::authoring;

namespace {

PortRef port(const std::string& text) { return PortRef::parse(text).value(); }

// 搭一个完整可发布的方案：输入 → 灰度 → 阈值 → Blob → 判定
struct Pipeline {
    Recipe recipe = Recipe::create("r-1", "测试方案");
    NodeId in, gray, thr, blob, judge;
    Pipeline() {
        in = recipe.addNode(test::imageInput()).value();
        gray = recipe.addNode(test::gray()).value();
        thr = recipe.addNode(test::threshold()).value();
        blob = recipe.addNode(test::blob()).value();
        judge = recipe.addNode(test::judge()).value();
        EXPECT_TRUE(recipe.connect(port(in + ".image"), port(gray + ".image")).ok());
        EXPECT_TRUE(recipe.connect(port(gray + ".image"), port(thr + ".image")).ok());
        EXPECT_TRUE(recipe.connect(port(thr + ".region"), port(blob + ".region")).ok());
        EXPECT_TRUE(recipe.connect(port(blob + ".blobs"), port(judge + ".blobs")).ok());
    }
};

}  // namespace

TEST(Recipe, CreateRecordsEvent) {
    Recipe r = Recipe::create("r-1", "测试");
    auto events = r.pullEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_STREQ(events[0]->name(), "RecipeCreated");
    EXPECT_TRUE(r.pullEvents().empty());
}

TEST(Recipe, NewNodeGetsDefaultParams) {
    Recipe r = Recipe::create("r-1", "测试");
    auto id = r.addNode(test::threshold()).value();
    EXPECT_EQ(std::get<double>(r.nodes()[0].params.at("value")), 128.0);
    EXPECT_EQ(id, "n1");
}

TEST(Recipe, R1_RejectsCycle) {
    Recipe r = Recipe::create("r-1", "测试");
    auto a = r.addNode(test::gray()).value();
    auto b = r.addNode(test::gray()).value();
    auto c = r.addNode(test::gray()).value();
    ASSERT_TRUE(r.connect(port(a + ".image"), port(b + ".image")).ok());
    ASSERT_TRUE(r.connect(port(b + ".image"), port(c + ".image")).ok());
    auto st = r.connect(port(c + ".image"), port(a + ".image"));
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, "authoring.cycle");
    EXPECT_FALSE(r.connect(port(a + ".image"), port(a + ".image")).ok()) << "自己连自己也是环";
}

TEST(Recipe, R2_RejectsTypeMismatchAndMissingPorts) {
    Recipe r = Recipe::create("r-1", "测试");
    auto in = r.addNode(test::imageInput()).value();
    auto blob = r.addNode(test::blob()).value();
    auto st = r.connect(port(in + ".image"), port(blob + ".region"));
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, "authoring.type_mismatch");
    EXPECT_EQ(r.connect(port(in + ".nope"), port(blob + ".region")).error().code, "authoring.port_not_found");
    EXPECT_EQ(r.connect(port("n99.image"), port(blob + ".region")).error().code, "authoring.node_not_found");
}

TEST(Recipe, R3_OneEdgePerInput) {
    Recipe r = Recipe::create("r-1", "测试");
    auto a = r.addNode(test::imageInput()).value();
    auto b = r.addNode(test::imageInput()).value();
    auto g = r.addNode(test::gray()).value();
    ASSERT_TRUE(r.connect(port(a + ".image"), port(g + ".image")).ok());
    EXPECT_EQ(r.connect(port(b + ".image"), port(g + ".image")).error().code, "authoring.input_occupied");
}

TEST(Recipe, R4_ValidatesParams) {
    Recipe r = Recipe::create("r-1", "测试");
    auto t = r.addNode(test::threshold()).value();
    EXPECT_TRUE(r.setParam(t, "value", 200.0).ok());
    EXPECT_EQ(r.setParam(t, "value", 300.0).error().code, "authoring.param_invalid");
    EXPECT_EQ(r.setParam(t, "value", true).error().code, "authoring.param_invalid");
    EXPECT_EQ(r.setParam(t, "method", std::string("magic")).error().code, "authoring.param_invalid");
    EXPECT_EQ(r.setParam(t, "nope", 1.0).error().code, "authoring.param_not_found");
    EXPECT_EQ(std::get<double>(r.nodes()[0].params.at("value")), 200.0) << "被拒绝的修改不能生效";
}

TEST(Recipe, R5_NamedRegionMustExistAndCannotBeRemovedWhileUsed) {
    Recipe r = Recipe::create("r-1", "测试");
    auto t = r.addNode(test::threshold()).value();
    EXPECT_EQ(r.setRoi(t, RoiSpec::named("r404")).error().code, "authoring.region_not_found");

    auto region = r.defineNamedRegion("标签区域", Roi::rect(10, 10, 50, 20).value()).value();
    ASSERT_TRUE(r.setRoi(t, RoiSpec::named(region)).ok());
    EXPECT_EQ(r.removeNamedRegion(region).error().code, "authoring.region_in_use");

    ASSERT_TRUE(r.setRoi(t, RoiSpec::none()).ok());
    EXPECT_TRUE(r.removeNamedRegion(region).ok());
}

TEST(Recipe, R6_RemoveNodeRemovesItsEdges) {
    Pipeline p;
    ASSERT_EQ(p.recipe.edges().size(), 4u);
    ASSERT_TRUE(p.recipe.removeNode(p.thr).ok());
    EXPECT_EQ(p.recipe.edges().size(), 2u) << "与被删节点相连的两条连线应一并删除";
    for (const auto& e : p.recipe.edges()) {
        EXPECT_NE(e.from.node, p.thr);
        EXPECT_NE(e.to.node, p.thr);
    }
}

TEST(Recipe, DraftMayBeIncompleteButPublishChecksP1) {
    Pipeline p;
    ASSERT_TRUE(p.recipe.removeNode(p.thr).ok());  // 草稿允许暂时不完整
    auto v = p.recipe.publish();
    ASSERT_FALSE(v.ok());
    EXPECT_EQ(v.error().code, "authoring.publish_rejected");
    EXPECT_NE(v.error().message.find("未连接"), std::string::npos);
}

TEST(Recipe, PublishChecksP5) {
    Recipe r = Recipe::create("r-1", "测试");
    auto in = r.addNode(test::imageInput()).value();
    auto g = r.addNode(test::gray()).value();
    ASSERT_TRUE(r.connect(port(in + ".image"), port(g + ".image")).ok());
    auto v = r.publish();
    ASSERT_FALSE(v.ok());
    EXPECT_NE(v.error().message.find("判定依据"), std::string::npos);
}

TEST(Recipe, PublishCreatesImmutableVersionAndEvent) {
    Pipeline p;
    p.recipe.pullEvents();
    auto v1 = p.recipe.publish();
    ASSERT_TRUE(v1.ok()) << v1.error().message;
    EXPECT_EQ(v1.value().id().str(), "r-1@v1");
    auto events = p.recipe.pullEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_STREQ(events[0]->name(), "RecipeVersionPublished");

    // 发布之后继续编辑草稿，已发布的版本不受影响
    ASSERT_TRUE(p.recipe.setParam(p.thr, "value", 10.0).ok());
    EXPECT_EQ(std::get<double>(v1.value().nodes()[2].params.at("value")), 128.0);

    auto v2 = p.recipe.publish();
    EXPECT_EQ(v2.value().id().str(), "r-1@v2");
}

TEST(RecipeVersion, CanOnlyBeDeprecatedOnce) {
    Pipeline p;
    auto v = p.recipe.publish().value();
    EXPECT_TRUE(v.deprecate().ok());
    EXPECT_EQ(v.status(), VersionStatus::Deprecated);
    EXPECT_FALSE(v.deprecate().ok());
}

TEST(RecipeVersion, ResolvesNamedRegionAndListsOperators) {
    Pipeline p;
    auto region = p.recipe.defineNamedRegion("区域A", Roi::rect(1, 2, 3, 4).value()).value();
    ASSERT_TRUE(p.recipe.setRoi(p.thr, RoiSpec::named(region)).ok());
    auto v = p.recipe.publish().value();
    EXPECT_EQ(*v.resolveRoi(v.nodes()[2]), Roi::rect(1, 2, 3, 4).value());
    EXPECT_EQ(v.operatorRefs().size(), 5u);
}

TEST(RecipeVersionId, ParsesText) {
    auto id = RecipeVersionId::parse("abc-123@v7");
    ASSERT_TRUE(id.ok());
    EXPECT_EQ(id.value().recipe, "abc-123");
    EXPECT_EQ(id.value().number, 7);
    EXPECT_FALSE(RecipeVersionId::parse("abc@vx").ok());
    EXPECT_FALSE(RecipeVersionId::parse("abc").ok());
}
