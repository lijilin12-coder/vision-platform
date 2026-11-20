// BC-4 上线管理领域层的单元测试（POC-9 的最小部分）。只链接领域库。
#include <gtest/gtest.h>

#include "vision/release/domain/release.h"

using namespace vision;
using namespace vision::release;

TEST(Release, CreateRecordsEventAndIsDeployable) {
    auto r = Release::create("rel-1", "recipe@v1");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().recipeVersionId(), "recipe@v1");
    EXPECT_TRUE(r.value().deployable());
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
