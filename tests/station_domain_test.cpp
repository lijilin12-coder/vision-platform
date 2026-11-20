// BC-5 工位运行领域层的单元测试：不变量 S1～S5、自动回滚（P7）。只链接领域库。
#include <gtest/gtest.h>

#include <algorithm>

#include "vision/station/domain/station.h"

using namespace vision;
using namespace vision::station;

namespace {

Station makeStation(int autoRollbackAfter = 3) {
    auto s = Station::create("st-1", "工位一", StationPolicy{autoRollbackAfter, 5});
    EXPECT_TRUE(s.ok());
    (void)s.value().pullEvents();
    return std::move(s).value();
}

// 部署一个上线包并让它就绪
void deployReady(Station& s, const std::string& release) {
    ASSERT_TRUE(s.deploy(release).ok());
    ASSERT_TRUE(s.markPendingReady(release).ok());
}

// 得到一个正在运行、生效上线包为 first 的工位
Station runningWith(const std::string& first, int autoRollbackAfter = 3) {
    Station s = makeStation(autoRollbackAfter);
    deployReady(s, first);
    EXPECT_TRUE(s.start().ok());
    (void)s.pullEvents();
    return s;
}

// 完成一次检测，返回它使用的上线包
std::string inspectOnce(Station& s, Outcome outcome = Outcome::Ok) {
    auto t = s.beginInspection();
    EXPECT_TRUE(t.ok());
    if (!t) return {};
    EXPECT_TRUE(s.recordOutcome(t.value(), outcome).ok());
    return t.value().releaseId;
}

std::vector<std::string> eventNames(Station& s) {
    std::vector<std::string> names;
    for (const auto& e : s.pullEvents()) names.emplace_back(e->name());
    return names;
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

// ---------- 创建 ----------

TEST(Station, CreateValidatesArguments) {
    EXPECT_FALSE(Station::create("", "工位").ok());
    EXPECT_FALSE(Station::create("st", "").ok());
    EXPECT_FALSE(Station::create("st", "工位", StationPolicy{0, 5}).ok());
    EXPECT_FALSE(Station::create("st", "工位", StationPolicy{3, 0}).ok());
}

// ---------- S1：运行中必须有生效的上线包 ----------

TEST(Station, S1_CannotStartWithoutRelease) {
    Station s = makeStation();
    const auto st = s.start();
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, "station.no_active_release");
    EXPECT_EQ(s.state(), StationState::Stopped);
}

TEST(Station, S1_CannotStartWhilePendingIsStillLoading) {
    Station s = makeStation();
    ASSERT_TRUE(s.deploy("A").ok());  // 还没有就绪
    EXPECT_FALSE(s.start().ok());
}

TEST(Station, StartActivatesReadyPending) {
    Station s = makeStation();
    deployReady(s, "A");
    ASSERT_TRUE(s.start().ok());
    EXPECT_EQ(s.state(), StationState::Running);
    EXPECT_EQ(s.activeRelease().value(), "A");
    EXPECT_FALSE(s.pendingRelease().has_value());
    EXPECT_TRUE(s.releaseHistory().empty());  // 第一次生效，没有“上一个”
}

TEST(Station, CannotInspectUnlessRunning) {
    Station s = makeStation();
    EXPECT_EQ(s.beginInspection().error().code, "station.not_running");

    s = runningWith("A");
    ASSERT_TRUE(s.pause().ok());
    EXPECT_EQ(s.beginInspection().error().code, "station.not_running");
    ASSERT_TRUE(s.resume().ok());
    EXPECT_TRUE(s.beginInspection().ok());
}

// ---------- S2：最多一个待切换的上线包 ----------

TEST(Station, S2_NewDeploymentReplacesPending) {
    Station s = runningWith("A");
    ASSERT_TRUE(s.deploy("B").ok());
    ASSERT_TRUE(s.deploy("C").ok());
    ASSERT_TRUE(s.pendingRelease().has_value());
    EXPECT_EQ(s.pendingRelease()->releaseId, "C");
    EXPECT_FALSE(s.pendingRelease()->ready);

    // B 的后台加载这时才完成：它已经不是待切换的上线包，必须被拒绝
    const auto late = s.markPendingReady("B");
    EXPECT_FALSE(late.ok());
    EXPECT_EQ(late.error().code, "station.pending_mismatch");
    EXPECT_FALSE(s.pendingRelease()->ready);

    // 接下来的检测仍然用 A：C 还没有就绪
    EXPECT_EQ(inspectOnce(s), "A");
}

TEST(Station, CannotDeployTheActiveRelease) {
    Station s = runningWith("A");
    EXPECT_EQ(s.deploy("A").error().code, "station.already_active");
}

TEST(Station, LoadFailureKeepsCurrentRelease) {
    Station s = runningWith("A");
    ASSERT_TRUE(s.deploy("B").ok());
    ASSERT_TRUE(s.markPendingFailed("B", "文件损坏").ok());
    EXPECT_FALSE(s.pendingRelease().has_value());
    EXPECT_EQ(s.state(), StationState::Running);
    EXPECT_EQ(inspectOnce(s), "A");
    EXPECT_TRUE(contains(eventNames(s), "ReleaseLoadFailed"));
}

// ---------- S3、S5：只在检测开始前切换；检测开始后上线包不再变化 ----------

TEST(Station, S3_SwapHappensOnlyAtBeginInspection) {
    Station s = runningWith("A");
    deployReady(s, "B");
    // 就绪并不等于切换：没有新的检测开始，生效的仍然是 A
    EXPECT_EQ(s.activeRelease().value(), "A");
    (void)s.pullEvents();

    auto t = s.beginInspection();
    ASSERT_TRUE(t.ok());
    EXPECT_TRUE(t.value().swapped);
    EXPECT_EQ(t.value().releaseId, "B");
    EXPECT_EQ(s.activeRelease().value(), "B");
    EXPECT_EQ(s.releaseHistory(), std::vector<std::string>{"A"});
    EXPECT_TRUE(contains(eventNames(s), "HotSwapped"));
}

TEST(Station, S5_InFlightInspectionKeepsItsRelease) {
    Station s = runningWith("A");

    // 一次检测已经开始，用的是 A
    auto inFlight = s.beginInspection();
    ASSERT_TRUE(inFlight.ok());
    EXPECT_EQ(inFlight.value().releaseId, "A");

    // 检测进行期间，新的上线包部署并且就绪了
    deployReady(s, "B");
    EXPECT_EQ(s.activeRelease().value(), "A") << "检测进行期间不能切换";
    EXPECT_EQ(inFlight.value().releaseId, "A") << "入场券上写的上线包不会变（H-04）";

    ASSERT_TRUE(s.recordOutcome(inFlight.value(), Outcome::Ok).ok());

    // 下一次检测开始前才切换
    auto next = s.beginInspection();
    ASSERT_TRUE(next.ok());
    EXPECT_TRUE(next.value().swapped);
    EXPECT_EQ(next.value().releaseId, "B");
    EXPECT_EQ(next.value().sequence, inFlight.value().sequence + 1);
}

TEST(Station, OneInspectionAtATime) {
    Station s = runningWith("A");
    auto first = s.beginInspection();
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(s.beginInspection().error().code, "station.inspection_in_progress");
    ASSERT_TRUE(s.recordOutcome(first.value(), Outcome::Ok).ok());
    EXPECT_EQ(s.recordOutcome(first.value(), Outcome::Ok).error().code, "station.no_inspection");
}

// ---------- S4：回滚的目标必须在历史中 ----------

TEST(Station, S4_CannotRollbackWithoutHistory) {
    Station s = runningWith("A");
    EXPECT_EQ(s.rollback(false).error().code, "station.no_rollback_target");
}

TEST(Station, ManualRollbackReturnsToPreviousRelease) {
    Station s = runningWith("A");
    deployReady(s, "B");
    EXPECT_EQ(inspectOnce(s), "B");
    (void)s.pullEvents();

    ASSERT_TRUE(s.rollback(true).ok());
    EXPECT_EQ(s.activeRelease().value(), "A");
    EXPECT_TRUE(s.releaseHistory().empty());
    EXPECT_EQ(inspectOnce(s), "A");

    const auto events = s.pullEvents();
    ASSERT_EQ(events.size(), 1u);
    const auto* rb = dynamic_cast<const RolledBack*>(events[0].get());
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(rb->fromRelease, "B");
    EXPECT_EQ(rb->toRelease, "A");
    EXPECT_EQ(rb->cause, RollbackCause::Manual);
    EXPECT_TRUE(rb->withdrawRequested);
}

TEST(Station, RollbackGoesBackStepByStep) {
    Station s = runningWith("A");
    deployReady(s, "B");
    EXPECT_EQ(inspectOnce(s), "B");
    deployReady(s, "C");
    EXPECT_EQ(inspectOnce(s), "C");
    EXPECT_EQ(s.releaseHistory(), (std::vector<std::string>{"A", "B"}));

    ASSERT_TRUE(s.rollback(false).ok());
    EXPECT_EQ(inspectOnce(s), "B");
    ASSERT_TRUE(s.rollback(false).ok());
    EXPECT_EQ(inspectOnce(s), "A");
    EXPECT_FALSE(s.rollback(false).ok());
}

TEST(Station, RollbackDiscardsPendingRelease) {
    Station s = runningWith("A");
    deployReady(s, "B");
    EXPECT_EQ(inspectOnce(s), "B");
    deployReady(s, "C");  // C 已就绪，但还没有开始新的检测
    (void)s.pullEvents();

    ASSERT_TRUE(s.rollback(false).ok());
    EXPECT_FALSE(s.pendingRelease().has_value()) << "否则下一次检测会立刻切换到 C，回滚等于没做";
    EXPECT_EQ(inspectOnce(s), "A");

    const auto events = s.pullEvents();
    const auto* rb = dynamic_cast<const RolledBack*>(events.at(0).get());
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(rb->discardedPending, "C");
}

TEST(Station, HistoryDepthIsLimited) {
    auto created = Station::create("st", "工位", StationPolicy{3, 2});
    ASSERT_TRUE(created.ok());
    Station s = std::move(created).value();
    deployReady(s, "A");
    ASSERT_TRUE(s.start().ok());
    for (const char* r : {"B", "C", "D"}) {
        deployReady(s, r);
        EXPECT_EQ(inspectOnce(s), r);
    }
    EXPECT_EQ(s.releaseHistory(), (std::vector<std::string>{"B", "C"})) << "只记住最近的 2 个";
}

// ---------- P7：热切换后连续 N 次 ERROR 自动回滚 ----------

TEST(Station, P7_AutoRollbackAfterConsecutiveErrors) {
    Station s = runningWith("A", 3);
    deployReady(s, "B");
    EXPECT_EQ(inspectOnce(s, Outcome::Error), "B");
    EXPECT_EQ(inspectOnce(s, Outcome::Error), "B");
    EXPECT_EQ(s.activeRelease().value(), "B") << "还没到 3 次";
    (void)s.pullEvents();

    EXPECT_EQ(inspectOnce(s, Outcome::Error), "B");
    EXPECT_EQ(s.activeRelease().value(), "A") << "第 3 次 ERROR 之后自动回滚";
    EXPECT_EQ(s.consecutiveErrors(), 0);
    EXPECT_EQ(s.state(), StationState::Running);

    const auto events = s.pullEvents();
    ASSERT_EQ(events.size(), 1u);
    const auto* rb = dynamic_cast<const RolledBack*>(events[0].get());
    ASSERT_NE(rb, nullptr);
    EXPECT_EQ(rb->cause, RollbackCause::Auto);
    EXPECT_FALSE(rb->withdrawRequested) << "是否撤回由人决定";

    EXPECT_EQ(inspectOnce(s), "A");
}

TEST(Station, ErrorsMustBeConsecutive) {
    Station s = runningWith("A", 3);
    deployReady(s, "B");
    inspectOnce(s, Outcome::Error);
    inspectOnce(s, Outcome::Error);
    inspectOnce(s, Outcome::Ng);  // NG 是正常的检测结果，不算 ERROR
    inspectOnce(s, Outcome::Error);
    inspectOnce(s, Outcome::Error);
    EXPECT_EQ(s.activeRelease().value(), "B");
    EXPECT_EQ(s.consecutiveErrors(), 2);
}

TEST(Station, SwapResetsErrorCount) {
    Station s = runningWith("A", 3);
    inspectOnce(s, Outcome::Error);
    inspectOnce(s, Outcome::Error);
    deployReady(s, "B");
    inspectOnce(s, Outcome::Error);  // 切换到 B 之后的第 1 次 ERROR
    EXPECT_EQ(s.activeRelease().value(), "B");
    EXPECT_EQ(s.consecutiveErrors(), 1);
}

TEST(Station, FaultsWhenErrorsPersistAndNothingToRollBackTo) {
    Station s = runningWith("A", 2);
    inspectOnce(s, Outcome::Error);
    (void)s.pullEvents();
    inspectOnce(s, Outcome::Error);
    EXPECT_EQ(s.state(), StationState::Faulted);
    EXPECT_TRUE(contains(eventNames(s), "StationFaulted"));
    EXPECT_FALSE(s.beginInspection().ok());

    // 部署一个好的上线包之后可以重新启动
    deployReady(s, "B");
    ASSERT_TRUE(s.start().ok());
    EXPECT_EQ(inspectOnce(s), "B");
}

TEST(Station, OutcomeOfOldReleaseDoesNotCountAfterManualRollback) {
    Station s = runningWith("A", 1);
    deployReady(s, "B");
    auto inFlight = s.beginInspection();  // 用 B
    ASSERT_TRUE(inFlight.ok());
    ASSERT_TRUE(s.rollback(false).ok());  // 检测进行期间，手动回滚到 A
    ASSERT_TRUE(s.recordOutcome(inFlight.value(), Outcome::Error).ok());
    EXPECT_EQ(s.consecutiveErrors(), 0) << "B 的 ERROR 不能算到 A 头上";
    EXPECT_EQ(s.activeRelease().value(), "A");
    EXPECT_EQ(s.state(), StationState::Running);
}

// ---------- 保存、恢复、重启 ----------

TEST(Station, SnapshotRoundTrip) {
    Station s = runningWith("A");
    deployReady(s, "B");
    inspectOnce(s);
    ASSERT_TRUE(s.deploy("C").ok());

    Station copy = Station::restore(s.snapshot());
    EXPECT_EQ(copy.id(), s.id());
    EXPECT_EQ(copy.state(), StationState::Running);
    EXPECT_EQ(copy.activeRelease().value(), "B");
    EXPECT_EQ(copy.pendingRelease()->releaseId, "C");
    EXPECT_EQ(copy.releaseHistory(), std::vector<std::string>{"A"});
    EXPECT_EQ(copy.sequence(), s.sequence());
    EXPECT_TRUE(copy.pullEvents().empty());
}

TEST(Station, WarmStartAfterRestart) {
    Station s = runningWith("A");
    deployReady(s, "B");
    inspectOnce(s);
    deployReady(s, "C");  // 就绪了但还没切换，这时程序被关掉

    Station restarted = Station::restore(s.snapshot());
    restarted.recoverAfterRestart();
    EXPECT_EQ(restarted.state(), StationState::Stopped);
    EXPECT_EQ(restarted.activeRelease().value(), "B") << "上次生效的上线包还在";
    ASSERT_TRUE(restarted.pendingRelease().has_value());
    EXPECT_FALSE(restarted.pendingRelease()->ready) << "加载好的内容随旧进程消失了，要重新加载";

    ASSERT_TRUE(restarted.start().ok());  // 热启动
    EXPECT_EQ(inspectOnce(restarted), "B");
}

TEST(Station, DirtyFlagOnlyOnPersistentChanges) {
    Station s = runningWith("A");
    (void)s.takeDirty();
    inspectOnce(s);  // 普通的一次检测：只有序号变了
    EXPECT_FALSE(s.takeDirty()) << "不值得为此写一次磁盘";
    ASSERT_TRUE(s.deploy("B").ok());
    EXPECT_TRUE(s.takeDirty());
    EXPECT_FALSE(s.takeDirty());
}
