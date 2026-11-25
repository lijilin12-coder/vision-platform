// BC-4 上线管理领域层的单元测试（POC-9 的最小部分）。只链接领域库。
#include <gtest/gtest.h>

#include "vision/release/domain/release.h"

using namespace vision;
using namespace vision::release;

TEST(Release, CreateRecordsEventAndIsDeployable) {
    auto r = Release::create("rel-1", "recipe@v1");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().recipeVersionId(), "recipe@v1");
    EXPECT_FALSE(r.value().deployable());  // L4：还没验收的不能部署（验收之后的见 acceptance_test.cpp）
    const auto events = r.value().pullEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_STREQ(events[0]->name(), "ReleaseCreated");
}

TEST(Release, CreateRequiresRecipeVersion) {
    EXPECT_FALSE(Release::create("rel-1", "").ok());
    EXPECT_FALSE(Release::create("", "recipe@v1").ok());
}

// L6：已撤回的上线包不能再部署；撤回不可撤销
TEST(Release, L6_WithdrawnIsNotDeployableAndCannotBeWithdrawnAgain) {
    auto r = Release::create("rel-1", "recipe@v1");
    ASSERT_TRUE(r.ok());
    (void)r.value().pullEvents();

    ASSERT_TRUE(r.value().withdraw().ok());
    EXPECT_FALSE(r.value().deployable());
    const auto events = r.value().pullEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_STREQ(events[0]->name(), "ReleaseWithdrawn");

    const auto again = r.value().withdraw();
    EXPECT_FALSE(again.ok());
    EXPECT_EQ(again.error().code, "release.already_withdrawn");
    EXPECT_TRUE(r.value().pullEvents().empty());
}

TEST(Release, RestoreKeepsStatusAndRecordsNoEvent) {
    auto r = Release::restore("rel-1", "recipe@v1", {{"detector", "scratch@v2.3"}}, ReleaseStatus::Withdrawn);
    EXPECT_FALSE(r.deployable());
    EXPECT_EQ(r.lockedModels().at("detector"), "scratch@v2.3");
    EXPECT_TRUE(r.pullEvents().empty());
}

// ---------------- 基准图集和评估规则（POC-11）----------------
#include "vision/release/domain/golden_set.h"

namespace {

GoldenSet frozenSet(AcceptanceCriteria criteria = {}) {
    auto set = GoldenSet::create("gs-1", "亮斑");
    EXPECT_TRUE(set.ok());
    GoldenSet s = std::move(set).value();
    for (const char* id : {"ok1", "ok2", "ok3", "ok4"}) EXPECT_TRUE(s.addCase({id, std::string(id) + ".png", Truth::Ok, {}}).ok());
    for (const char* id : {"ng1", "ng2"}) EXPECT_TRUE(s.addCase({id, std::string(id) + ".png", Truth::Ng, {}}).ok());
    EXPECT_TRUE(s.setCriteria(criteria).ok());
    EXPECT_TRUE(s.freeze().ok());
    return s;
}

}  // namespace

TEST(GoldenSet, FrozenSetCannotBeChanged) {
    GoldenSet s = frozenSet();
    EXPECT_TRUE(s.frozen());
    EXPECT_FALSE(s.addCase({"x", "x.png", Truth::Ok, {}}).ok());
    EXPECT_FALSE(s.setCriteria({1, 0.5, 0}).ok());
    EXPECT_FALSE(s.freeze().ok());
}

TEST(GoldenSet, RejectsDuplicateCasesAndEmptySets) {
    auto set = GoldenSet::create("gs", "n");
    ASSERT_TRUE(set.ok());
    EXPECT_FALSE(set.value().freeze().ok());  // 空的图集不能冻结
    EXPECT_TRUE(set.value().addCase({"a", "a.png", Truth::Ok, {}}).ok());
    EXPECT_FALSE(set.value().addCase({"a", "b.png", Truth::Ng, {}}).ok());
    EXPECT_FALSE(set.value().setCriteria({-1, 0, 0}).ok());
    EXPECT_FALSE(set.value().setCriteria({0, 1.5, 0}).ok());
}

TEST(Evaluation, CountsMissesFalseAlarmsAndErrors) {
    const GoldenSet s = frozenSet();
    auto e = evaluateCases(s, {{"ok1", Observed::Ok, 1.0}, {"ok2", Observed::Ng, 3.0}, {"ok3", Observed::Error, 1.0},
                               {"ok4", Observed::Ok, 1.0}, {"ng1", Observed::Ng, 1.0}, {"ng2", Observed::Ok, 5.0}});
    ASSERT_TRUE(e.ok());
    EXPECT_EQ(e.value().truthOk, 4);
    EXPECT_EQ(e.value().truthNg, 2);
    EXPECT_EQ(e.value().falseAlarms, 1);  // ok2 被判成不合格
    EXPECT_EQ(e.value().misses, 1);       // ng2 被判成合格
    EXPECT_EQ(e.value().errors, 1);
    EXPECT_DOUBLE_EQ(e.value().maxMs, 5.0);
    EXPECT_DOUBLE_EQ(e.value().meanMs, 2.0);
    EXPECT_EQ(e.value().wrongCaseIds.size(), 3u);
}

TEST(Evaluation, RejectsUnknownOrRepeatedCases) {
    const GoldenSet s = frozenSet();
    EXPECT_FALSE(evaluateCases(s, {{"nope", Observed::Ok, 1.0}}).ok());
    EXPECT_FALSE(evaluateCases(s, {{"ok1", Observed::Ok, 1.0}, {"ok1", Observed::Ok, 1.0}}).ok());
    EXPECT_TRUE(evaluateCases(s, {{"ok1", Observed::Ok, 1.0}}).ok());  // 只评估一部分是允许的
}

TEST(Evaluation, MeetsCriteria) {
    CaseEvaluation e;
    e.truthOk = 10;
    e.truthNg = 5;
    EXPECT_TRUE(meets(e, {0, 0.0, 0}));
    e.falseAlarms = 1;
    EXPECT_FALSE(meets(e, {0, 0.0, 0}));
    EXPECT_TRUE(meets(e, {0, 0.1, 0}));   // 10 个里误报 1 个，正好 10%
    EXPECT_FALSE(meets(e, {0, 0.05, 0}));
    e.misses = 1;
    EXPECT_FALSE(meets(e, {0, 0.1, 0}));
    EXPECT_TRUE(meets(e, {1, 0.1, 0}));
    e.errors = 1;
    EXPECT_FALSE(meets(e, {1, 0.1, 0}));
}

// 比较两次评估：先看出错，再看漏检，最后看误报。漏检比误报严重：坏件流出去的代价更大。
TEST(Evaluation, BetterThanOrdersByErrorsThenMissesThenFalseAlarms) {
    const auto make = [](int errors, int misses, int falseAlarms) {
        CaseEvaluation e;
        e.errors = errors;
        e.misses = misses;
        e.falseAlarms = falseAlarms;
        return e;
    };
    EXPECT_TRUE(betterThan(make(0, 5, 5), make(1, 0, 0)));
    EXPECT_TRUE(betterThan(make(0, 0, 9), make(0, 1, 0)));
    EXPECT_TRUE(betterThan(make(0, 1, 2), make(0, 1, 3)));
    EXPECT_FALSE(betterThan(make(0, 1, 2), make(0, 1, 2)));  // 一样好不算更好
    EXPECT_FALSE(betterThan(make(0, 1, 0), make(0, 0, 9)));
}
