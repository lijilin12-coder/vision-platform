// 工位运行的集成测试（POC-9）：真实算子 + 检测引擎 + JSON 文件仓储 + 后台加载线程。
// 重点：热切换不丢失、不中断任何检测；每次检测用的是它开始时生效的上线包（H-04）。

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <thread>

#include "vision/authoring/api.h"
#include "vision/catalog/api.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/execution/api.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/persistence/json_repositories.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/release/api.h"
#include "vision/station/api.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

// 一张小的产品图像，上面有 spots 个亮斑
Image makeProduct(int spots) {
    cv::Mat m(120, 160, CV_8UC3, cv::Scalar(150, 150, 150));
    cv::rectangle(m, cv::Rect(20, 20, 120, 80), cv::Scalar(70, 70, 70), cv::FILLED);
    for (int i = 0; i < spots; ++i) cv::circle(m, cv::Point(50 + 40 * i, 60), 5, cv::Scalar(240, 240, 240), cv::FILLED);
    return imaging::fromMat(m).value();
}

// 方案版本的来源：正常时转给方案编排；可以设定某个版本“取不到”，用来模拟上线包加载失败。
class TestRecipeSource final : public release::RecipeVersionSource {
public:
    explicit TestRecipeSource(const authoring::AuthoringService& a) : authoring_(a) {}
    Result<authoring::RecipeVersionDocument> find(const std::string& versionId) const override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (versionId == failing_) {
                return Result<authoring::RecipeVersionDocument>::failure("test.unavailable", "模拟：方案版本文件损坏");
            }
        }
        return authoring_.getVersionDocument(versionId);
    }
    void failFor(const std::string& versionId) {
        std::lock_guard<std::mutex> lock(mutex_);
        failing_ = versionId;
    }

private:
    const authoring::AuthoringService& authoring_;
    mutable std::mutex mutex_;
    std::string failing_;
};

// 一套完整的服务。测试“重启”时会销毁再重建它，数据目录不变。
struct Services {
    eventbus::InMemoryEventBus bus;
    std::mutex eventsMutex;
    std::vector<std::string> events;
    persistence::JsonRecipeRepository recipes;
    persistence::JsonRecipeVersionRepository versions;
    persistence::JsonReleaseRepository releaseRepo;
    persistence::JsonStationRepository stationRepo;
    authoring::AuthoringService authoring;
    execution::ExecutionService execution;
    TestRecipeSource source;
    release::ReleaseService releases;
    station::StationService stations;

    explicit Services(const fs::path& dir)
        : recipes(dir),
          versions(dir),
          releaseRepo(dir),
          stationRepo(dir),
          authoring(recipes, versions, catalog::OperatorCatalog::global(), bus),
          execution(catalog::OperatorCatalog::global(), bus),
          source(authoring),
          releases(releaseRepo, source, bus),
          stations(stationRepo, releases, execution, bus) {
        bus.subscribe("*", [this](const EventPtr& e) {
            std::lock_guard<std::mutex> lock(eventsMutex);
            events.emplace_back(e->name());
        });
    }

    int count(const std::string& name) {
        std::lock_guard<std::mutex> lock(eventsMutex);
        return static_cast<int>(std::count(events.begin(), events.end(), name));
    }
};

class StationPlatform : public ::testing::Test {
protected:
    fs::path dir = fs::temp_directory_path() / ("vision-st-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                                                "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::unique_ptr<Services> s;

    std::string strictVersion, looseVersion, brokenVersion;  // 方案版本
    std::string strict, loose, broken;                       // 对应的上线包

    void SetUp() override {
        fs::remove_all(dir);
        s = std::make_unique<Services>(dir);
        // 三个方案，对同一张“有 2 个亮斑”的图像给出三种不同的判定：
        strictVersion = publish(0, true);   // 上限 0 个 → NG
        looseVersion = publish(5, true);    // 上限 5 个 → OK
        brokenVersion = publish(0, false);  // 少了“灰度”节点，阈值算子拒绝彩色图像 → ERROR
        strict = s->releases.createRelease(strictVersion).value();
        loose = s->releases.createRelease(looseVersion).value();
        broken = s->releases.createRelease(brokenVersion).value();
    }
    void TearDown() override {
        s.reset();
        fs::remove_all(dir);
    }

    // 输入 →（灰度）→ 阈值(200) → Blob(面积≥20) → 计数判定(上限 maxCount)
    std::string publish(int maxCount, bool withGray) {
        auto& a = s->authoring;
        auto rid = a.createRecipe("亮斑检测").value();
        auto in = a.addNode(rid, "vision.image_input").value();
        auto thr = a.addNode(rid, "vision.threshold").value();
        auto blob = a.addNode(rid, "vision.blob").value();
        auto judge = a.addNode(rid, "vision.judge_count").value();
        EXPECT_TRUE(a.setParam(rid, thr, "value", 200.0).ok());
        EXPECT_TRUE(a.setParam(rid, blob, "minArea", 20.0).ok());
        EXPECT_TRUE(a.setParam(rid, judge, "maxCount", static_cast<double>(maxCount)).ok());
        if (withGray) {
            auto gray = a.addNode(rid, "vision.to_gray").value();
            EXPECT_TRUE(a.connect(rid, in + ".image", gray + ".image").ok());
            EXPECT_TRUE(a.connect(rid, gray + ".image", thr + ".image").ok());
        } else {
            EXPECT_TRUE(a.connect(rid, in + ".image", thr + ".image").ok());
        }
        EXPECT_TRUE(a.connect(rid, thr + ".region", blob + ".region").ok());
        EXPECT_TRUE(a.connect(rid, blob + ".blobs", judge + ".blobs").ok());
        auto vid = a.publish(rid);
        EXPECT_TRUE(vid.ok()) << (vid.ok() ? "" : vid.error().message);
        return vid.value();
    }

    // 建一个工位，部署第一个上线包并启动
    std::string runningStation(const std::string& firstRelease, int autoRollbackAfter = 3) {
        auto id = s->stations.createStation("工位一", {autoRollbackAfter, 5}).value();
        EXPECT_TRUE(s->stations.deploy(id, firstRelease).ok());
        EXPECT_TRUE(s->stations.waitForLoading(id).ok());
        auto st = s->stations.start(id);
        EXPECT_TRUE(st.ok()) << (st.ok() ? "" : st.error().message);
        return id;
    }

    void deployAndWait(const std::string& stationId, const std::string& releaseId) {
        auto st = s->stations.deploy(stationId, releaseId);
        ASSERT_TRUE(st.ok()) << st.error().message;
        ASSERT_TRUE(s->stations.waitForLoading(stationId).ok());
    }

    station::StationInspection inspect(const std::string& stationId, int spots = 2) {
        auto r = s->stations.inspect(stationId, makeProduct(spots));
        EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().message);
        return r.ok() ? r.value() : station::StationInspection{};
    }
};

}  // namespace

TEST_F(StationPlatform, DeployStartInspect) {
    auto id = runningStation(strict);
    auto view = s->stations.getStation(id).value();
    EXPECT_EQ(view.state, "running");
    EXPECT_EQ(view.activeRelease, strict);

    auto r = inspect(id);
    EXPECT_EQ(r.releaseId, strict);
    EXPECT_EQ(r.report.verdict, "NG");
    EXPECT_EQ(r.report.recipeVersionId, strictVersion);
    EXPECT_EQ(r.sequence, 1u);
    EXPECT_EQ(inspect(id, 0).report.verdict, "OK");
}

TEST_F(StationPlatform, CannotStartOrInspectWithoutRelease) {
    auto id = s->stations.createStation("空工位").value();
    EXPECT_EQ(s->stations.start(id).error().code, "station.no_active_release");
    EXPECT_EQ(s->stations.inspect(id, makeProduct(0)).error().code, "station.not_running");
    EXPECT_EQ(s->stations.start("no-such-station").error().code, "station.not_found");
}

TEST_F(StationPlatform, HotSwapTakesEffectAtNextInspection) {
    auto id = runningStation(strict);
    EXPECT_EQ(inspect(id).report.verdict, "NG");

    deployAndWait(id, loose);
    auto before = s->stations.getStation(id).value();
    EXPECT_EQ(before.activeRelease, strict) << "就绪之后、下一次检测开始之前，生效的仍然是旧的";
    EXPECT_EQ(before.pendingRelease, loose);
    EXPECT_TRUE(before.pendingReady);

    auto r = inspect(id);
    EXPECT_TRUE(r.swapped);
    EXPECT_EQ(r.releaseId, loose);
    EXPECT_EQ(r.report.verdict, "OK");
    EXPECT_EQ(r.report.recipeVersionId, looseVersion);
    EXPECT_GT(r.swapMicros, 0.0);

    auto after = s->stations.getStation(id).value();
    EXPECT_EQ(after.activeRelease, loose);
    EXPECT_EQ(after.releaseHistory, std::vector<std::string>{strict});
    EXPECT_EQ(s->count("ReleaseDeployed"), 2);
    EXPECT_EQ(s->count("ReleaseReady"), 2);
    EXPECT_EQ(s->count("HotSwapped"), 2) << "第一次生效也记为一次切换";
}

TEST_F(StationPlatform, AutoRollbackAfterConsecutiveErrors) {
    auto id = runningStation(strict, 3);
    deployAndWait(id, broken);

    for (int i = 0; i < 3; ++i) {
        auto r = inspect(id);
        EXPECT_EQ(r.releaseId, broken);
        EXPECT_EQ(r.report.verdict, "ERROR");
    }
    // 第 3 次 ERROR 之后已经自动回滚，接下来的检测恢复正常
    auto r = inspect(id);
    EXPECT_EQ(r.releaseId, strict);
    EXPECT_EQ(r.report.verdict, "NG");
    EXPECT_EQ(s->count("RolledBack"), 1);
    EXPECT_EQ(s->stations.getStation(id).value().state, "running");
    EXPECT_FALSE(s->releases.getManifest(broken).value().withdrawn) << "自动回滚不撤回上线包，由人决定";
}

TEST_F(StationPlatform, ManualRollbackCanWithdrawTheRelease) {
    auto id = runningStation(strict);
    deployAndWait(id, loose);
    EXPECT_EQ(inspect(id).releaseId, loose);

    ASSERT_TRUE(s->stations.rollback(id, true).ok());
    EXPECT_EQ(inspect(id).releaseId, strict);
    EXPECT_TRUE(s->releases.getManifest(loose).value().withdrawn);
    EXPECT_EQ(s->count("ReleaseWithdrawn"), 1);

    // 已撤回的上线包不能再部署（L6）
    EXPECT_EQ(s->stations.deploy(id, loose).error().code, "station.release_withdrawn");
    EXPECT_EQ(s->stations.rollback(id).error().code, "station.no_rollback_target");
}

TEST_F(StationPlatform, DeployRejectsUnknownRelease) {
    auto id = runningStation(strict);
    EXPECT_EQ(s->stations.deploy(id, "no-such-release").error().code, "release.not_found");
    EXPECT_EQ(s->stations.deploy(id, strict).error().code, "station.already_active");
}

TEST_F(StationPlatform, LoadFailureDoesNotAffectProduction) {
    auto id = runningStation(strict);
    s->source.failFor(looseVersion);  // loose 的内容“损坏”了
    deployAndWait(id, loose);

    auto view = s->stations.getStation(id).value();
    EXPECT_EQ(view.pendingRelease, "") << "加载失败的部署被丢弃";
    EXPECT_EQ(view.state, "running");
    EXPECT_EQ(s->count("ReleaseLoadFailed"), 1);
    auto r = inspect(id);
    EXPECT_FALSE(r.swapped);
    EXPECT_EQ(r.releaseId, strict);
}

TEST_F(StationPlatform, WarmStartAfterRestart) {
    auto id = runningStation(strict);
    deployAndWait(id, loose);
    EXPECT_EQ(inspect(id).releaseId, loose);

    // “重启”：销毁全部服务，用同一个数据目录重新建立
    s = std::make_unique<Services>(dir);
    auto view = s->stations.getStation(id).value();
    EXPECT_EQ(view.state, "stopped");
    EXPECT_EQ(view.activeRelease, loose) << "上次生效的上线包保存下来了";
    EXPECT_EQ(view.releaseHistory, std::vector<std::string>{strict});

    ASSERT_TRUE(s->stations.start(id).ok());  // 热启动：不需要重新部署
    auto r = inspect(id);
    EXPECT_EQ(r.releaseId, loose);
    EXPECT_EQ(r.report.verdict, "OK");

    // 重启之后回滚到一个本次运行还没用过的上线包，也能正常检测
    ASSERT_TRUE(s->stations.rollback(id).ok());
    r = inspect(id);
    EXPECT_EQ(r.releaseId, strict);
    EXPECT_EQ(r.report.verdict, "NG");
}

TEST_F(StationPlatform, PendingDeploymentIsReloadedAfterRestart) {
    auto id = runningStation(strict);
    deployAndWait(id, loose);  // 就绪了，但还没有新的检测，所以没有切换

    s = std::make_unique<Services>(dir);
    ASSERT_TRUE(s->stations.waitForLoading(id).ok());  // 第一次用到这个工位时，会在后台重新加载
    auto view = s->stations.getStation(id).value();
    EXPECT_EQ(view.activeRelease, strict);
    EXPECT_EQ(view.pendingRelease, loose);
    EXPECT_TRUE(view.pendingReady);

    ASSERT_TRUE(s->stations.start(id).ok());  // 启动时，就绪的待切换上线包直接生效
    EXPECT_EQ(inspect(id).releaseId, loose);
}

TEST_F(StationPlatform, TwoStationsAreIndependent) {
    auto a = runningStation(strict);
    auto b = runningStation(loose);
    EXPECT_EQ(inspect(a).report.verdict, "NG");
    EXPECT_EQ(inspect(b).report.verdict, "OK");
    deployAndWait(a, loose);
    EXPECT_EQ(inspect(a).releaseId, loose);
    EXPECT_EQ(s->stations.getStation(b).value().releaseHistory.size(), 0u);
    EXPECT_EQ(inspect(b).sequence, 2u);
}

// POC-9 的通过标准：连续检测 10,000 张图，期间 20 次部署和切换、5 次回滚，
// 没有丢失或中断任何检测；每次检测用的上线包就是它开始时生效的那一个。
TEST_F(StationPlatform, HotSwapUnderLoad_NoInspectionLostOrMixed) {
    const int total = 10000;
    const int deployments = 20;
    const int rollbacks = 5;

    auto id = runningStation(strict);
    const Image twoSpots = makeProduct(2);
    const Image clean = makeProduct(0);

    std::atomic<int> done{0};
    std::atomic<bool> controllerFailed{false};

    // 控制线程：在检测进行的同时，不断部署和回滚。它和检测线程之间没有任何约定的时机。
    std::thread controller([&] {
        int deployed = 0, rolledBack = 0;
        const int steps = deployments + rollbacks;
        for (int step = 1; step <= steps; ++step) {
            // 把操作大致均匀地分散在整个过程中
            const int at = step * (total - 500) / steps;
            while (done.load() < at) std::this_thread::sleep_for(std::chrono::microseconds(200));

            const bool doRollback = (step % 5 == 0) && rolledBack < rollbacks;
            if (doRollback) {
                // 历史为空时回滚会被拒绝，那是正常的业务拒绝，换成一次部署即可
                if (s->stations.rollback(id).ok()) {
                    ++rolledBack;
                    continue;
                }
            }
            const auto view = s->stations.getStation(id).value();
            const std::string next = view.activeRelease == strict ? loose : strict;
            if (!s->stations.deploy(id, next).ok()) controllerFailed = true;
            ++deployed;
        }
    });

    int swaps = 0, lost = 0, mixed = 0, wrongVerdict = 0, outOfOrder = 0;
    std::uint64_t lastSequence = 0;
    for (int i = 0; i < total; ++i) {
        const bool defective = (i % 3 != 0);
        auto r = s->stations.inspect(id, defective ? twoSpots : clean);
        if (!r) {
            ++lost;
            done.fetch_add(1);
            continue;
        }
        const auto& v = r.value();
        if (v.swapped) ++swaps;
        if (v.sequence != lastSequence + 1) ++outOfOrder;
        lastSequence = v.sequence;

        // 这次检测“应该”用的方案版本和判定，完全由它入场时拿到的上线包决定
        const bool isStrict = v.releaseId == strict;
        const std::string expectedVersion = isStrict ? strictVersion : looseVersion;
        const std::string expectedVerdict = (defective && isStrict) ? "NG" : "OK";
        if (v.releaseId != strict && v.releaseId != loose) ++mixed;
        if (v.report.recipeVersionId != expectedVersion) ++mixed;
        if (v.report.verdict != expectedVerdict) ++wrongVerdict;
        done.fetch_add(1);
    }
    controller.join();
    ASSERT_TRUE(s->stations.waitForLoading(id).ok());

    EXPECT_EQ(lost, 0) << "有检测丢失或被中断";
    EXPECT_EQ(mixed, 0) << "有检测用了不属于它的上线包的方案";
    EXPECT_EQ(wrongVerdict, 0) << "有检测的判定和它的上线包不符";
    EXPECT_EQ(outOfOrder, 0) << "检测序号不连续";
    EXPECT_FALSE(controllerFailed.load());
    EXPECT_EQ(lastSequence, static_cast<std::uint64_t>(total));
    EXPECT_GE(swaps, 10) << "切换次数太少，说明测试没有真正压到热切换";
    EXPECT_EQ(s->count("HotSwapped"), swaps + 1) << "事件里的切换次数应当等于检测看到的切换次数（加上第一次生效）";
    EXPECT_EQ(s->count("InspectionCompleted"), total);
    EXPECT_GE(s->count("RolledBack"), 1);
    EXPECT_EQ(s->count("ReleaseLoadFailed"), 0);
    EXPECT_EQ(s->stations.getStation(id).value().state, "running");

    std::cout << "      " << total << " 次检测，" << swaps << " 次热切换，" << s->count("RolledBack") << " 次回滚，"
              << s->count("ReleaseDeployed") << " 次部署\n";
}
