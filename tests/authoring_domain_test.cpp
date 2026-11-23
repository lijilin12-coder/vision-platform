// 方案编排领域层测试：不变量 R1～R6、发布规则 P1 / P5、方案版本。
// 只链接 authoring_domain（不链接 OpenCV，不需要图像），毫秒级跑完。

#include <gtest/gtest.h>

#include <functional>

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

// ---------------- 匹配模板：R7、R8、P6（docs/ddd/09-match-template-and-locating.md）----------------

namespace {

TemplateSpec spec(const std::string& name = "定位孔") {
    TemplateSpec t;
    t.name = name;
    t.sha256 = std::string(64, 'a');
    t.bytes = 1234;
    t.width = 96;
    t.height = 64;
    t.referenceX = 200;
    t.referenceY = 100;
    t.sourceWidth = 640;
    t.sourceHeight = 480;
    return t;
}

// 在完整的流水线里插一个带模板参数的节点：输入 → 灰度 → 定位 → 阈值 → …
struct LocatingPipeline : Pipeline {
    NodeId locate;
    LocatingPipeline() {
        locate = recipe.addNode(test::locate()).value();
        EXPECT_TRUE(recipe.disconnect(port(gray + ".image"), port(thr + ".image")).ok());
        EXPECT_TRUE(recipe.connect(port(gray + ".image"), port(locate + ".image")).ok());
        EXPECT_TRUE(recipe.connect(port(locate + ".image"), port(thr + ".image")).ok());
    }
};

}  // namespace

TEST(MatchTemplate, AddGivesAnIdAndKeepsTheSpec) {
    Recipe r = Recipe::create("r-1", "测试");
    (void)r.pullEvents();
    auto id = r.addMatchTemplate(spec());
    ASSERT_TRUE(id.ok());
    EXPECT_EQ(id.value(), "t1");
    ASSERT_EQ(r.templates().size(), 1u);
    EXPECT_EQ(r.templates()[0].spec.name, "定位孔");
    EXPECT_EQ(r.templates()[0].spec.referenceX, 200);
    EXPECT_TRUE(r.pullEvents().empty());  // 编辑草稿不对外发布事件
    EXPECT_EQ(r.addMatchTemplate(spec("另一个")).value(), "t2");
}

// R8：名字唯一；至少 8×8；完全落在样图之内
TEST(MatchTemplate, R8_NameAndSize) {
    Recipe r = Recipe::create("r-1", "测试");
    ASSERT_TRUE(r.addMatchTemplate(spec()).ok());
    EXPECT_EQ(r.addMatchTemplate(spec()).error().code, "authoring.template_name");    // 重名
    EXPECT_EQ(r.addMatchTemplate(spec("")).error().code, "authoring.template_name");  // 没有名字

    const auto rejected = [&r](const std::function<void(TemplateSpec&)>& change) {
        TemplateSpec t = spec("x");
        change(t);
        return r.checkNewMatchTemplate(t).error().code;
    };
    EXPECT_EQ(rejected([](TemplateSpec& t) { t.width = 7; }), "authoring.template_size");
    EXPECT_EQ(rejected([](TemplateSpec& t) { t.height = 0; }), "authoring.template_size");
    EXPECT_EQ(rejected([](TemplateSpec& t) { t.referenceX = 600; }), "authoring.template_size");  // 右边伸出了样图
    EXPECT_EQ(rejected([](TemplateSpec& t) { t.referenceY = -1; }), "authoring.template_size");
    EXPECT_EQ(rejected([](TemplateSpec& t) { t.sourceWidth = 0; }), "authoring.template_size");
    // 正好贴着样图的边是可以的
    TemplateSpec edge = spec("贴边");
    edge.referenceX = 640 - 96;
    edge.referenceY = 480 - 64;
    EXPECT_TRUE(r.checkNewMatchTemplate(edge).ok());
    EXPECT_EQ(r.templates().size(), 1u);  // check 只检查，不添加

    TemplateSpec noContent = spec("没有内容");
    noContent.sha256.clear();
    EXPECT_EQ(r.addMatchTemplate(noContent).error().code, "authoring.template_content");
}

TEST(MatchTemplate, Rename) {
    Recipe r = Recipe::create("r-1", "测试");
    const auto a = r.addMatchTemplate(spec("甲")).value();
    const auto b = r.addMatchTemplate(spec("乙")).value();
    EXPECT_TRUE(r.renameMatchTemplate(a, "丙").ok());
    EXPECT_EQ(r.templates()[0].spec.name, "丙");
    EXPECT_TRUE(r.renameMatchTemplate(a, "丙").ok());  // 改成自己现在的名字不算重名
    EXPECT_EQ(r.renameMatchTemplate(b, "丙").error().code, "authoring.template_name");
    EXPECT_EQ(r.renameMatchTemplate(b, "").error().code, "authoring.template_name");
    EXPECT_EQ(r.renameMatchTemplate("t9", "x").error().code, "authoring.template_not_found");
}

// R7：模板参数必须指向存在的模板；被引用的模板不能删除
TEST(MatchTemplate, R7_ReferenceMustExistAndReferencedTemplateCannotBeRemoved) {
    LocatingPipeline p;
    const auto t = p.recipe.addMatchTemplate(spec()).value();

    EXPECT_EQ(p.recipe.setParam(p.locate, "template", std::string("t9")).error().code, "authoring.template_not_found");
    EXPECT_EQ(p.recipe.setParam(p.locate, "template", 3.0).error().code, "authoring.param_invalid");  // 必须是文本
    ASSERT_TRUE(p.recipe.setParam(p.locate, "template", t).ok());

    EXPECT_EQ(p.recipe.removeMatchTemplate(t).error().code, "authoring.template_in_use");
    ASSERT_TRUE(p.recipe.setParam(p.locate, "template", std::string("")).ok());  // 取消引用（草稿里允许为空）
    EXPECT_TRUE(p.recipe.removeMatchTemplate(t).ok());
    EXPECT_EQ(p.recipe.removeMatchTemplate(t).error().code, "authoring.template_not_found");

    // 删掉引用它的节点之后，模板也可以删除
    const auto t2 = p.recipe.addMatchTemplate(spec()).value();
    ASSERT_TRUE(p.recipe.setParam(p.locate, "template", t2).ok());
    ASSERT_TRUE(p.recipe.removeNode(p.locate).ok());
    EXPECT_TRUE(p.recipe.removeMatchTemplate(t2).ok());
}

// 普通的文本参数不受 R7 影响：只有“模板”类型的参数才检查
TEST(MatchTemplate, R7_OnlyAppliesToTemplateParams) {
    Recipe r = Recipe::create("r-1", "测试");
    const auto n = r.addNode(test::threshold()).value();
    EXPECT_TRUE(r.setParam(n, "method", std::string("otsu")).ok());
}

// P6：发布时每个模板参数都必须已经设置
TEST(MatchTemplate, P6_PublishRequiresTemplateParamsToBeSet) {
    LocatingPipeline p;
    auto rejected = p.recipe.publish();
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "authoring.publish_rejected");
    EXPECT_NE(rejected.error().message.find("模板参数"), std::string::npos);

    const auto t = p.recipe.addMatchTemplate(spec()).value();
    ASSERT_TRUE(p.recipe.setParam(p.locate, "template", t).ok());
    EXPECT_TRUE(p.recipe.publish().ok());
}

// 模板随方案版本一起冻结：发布之后草稿里怎么改，已发布的版本都不变
TEST(MatchTemplate, FrozenWithThePublishedVersion) {
    LocatingPipeline p;
    const auto t = p.recipe.addMatchTemplate(spec()).value();
    ASSERT_TRUE(p.recipe.setParam(p.locate, "template", t).ok());
    const RecipeVersion v1 = p.recipe.publish().value();
    ASSERT_EQ(v1.templates().size(), 1u);
    EXPECT_EQ(v1.templates()[0].spec.sha256, std::string(64, 'a'));

    // 草稿：换一个模板，旧的改名、删掉
    TemplateSpec other = spec("新的");
    other.sha256 = std::string(64, 'b');
    const auto t2 = p.recipe.addMatchTemplate(other).value();
    ASSERT_TRUE(p.recipe.setParam(p.locate, "template", t2).ok());
    ASSERT_TRUE(p.recipe.removeMatchTemplate(t).ok());
    const RecipeVersion v2 = p.recipe.publish().value();

    EXPECT_EQ(v1.templates()[0].spec.name, "定位孔");  // v1 没有跟着变
    EXPECT_EQ(v1.templates()[0].spec.sha256, std::string(64, 'a'));
    ASSERT_EQ(v2.templates().size(), 1u);
    EXPECT_EQ(v2.templates()[0].spec.sha256, std::string(64, 'b'));
}

// 没有模板的方案和以前完全一样
TEST(MatchTemplate, RecipesWithoutTemplatesAreUnaffected) {
    Pipeline p;
    const auto v = p.recipe.publish();
    ASSERT_TRUE(v.ok());
    EXPECT_TRUE(v.value().templates().empty());
}
