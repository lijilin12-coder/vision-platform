// 检测记录（BC-10 质量分析，docs/ddd/14-inspection-records.md）
//
//   1. 领域层：不变量 Q2、Q3，编号，汇总
//   2. 存储：每个工位每天一个文件，一行一条
//   3. 应用服务：编号递增、留图策略
//   4. 整条路：工位检测一次，磁盘上就多一条记录；程序重启之后还查得到

#include <gtest/gtest.h>

#include <filesystem>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>

#include "platform.h"
#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/quality/api.h"
#include "vision/quality/domain/inspection_record.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

const std::int64_t kDay = 86400000;
const std::int64_t kNoon = 1795521600000;  // 2026-11-24 12:00:00 UTC

fs::path freshDir(const std::string& name) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const fs::path dir = fs::temp_directory_path() / pathFromUtf8("vision-检测记录-" + std::string(info->name()) + "-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

quality::InspectionRecord::Fields fields(std::int64_t timeMs, std::uint64_t counter, const std::string& verdict) {
    quality::InspectionRecord::Fields f;
    f.id = quality::makeRecordId(timeMs, counter);
    f.stationId = "s-1";
    f.timeMs = timeMs;
    f.sequence = counter + 1;
    f.releaseId = "rel-1";
    f.recipeVersionId = "recipe@v1";
    f.frameName = "图像.png";
    f.verdict = verdict;
    if (verdict == "NG") {
        f.reasons.push_back({"n5", false, "亮斑 2 个，超过上限 0"});
        Defect d;
        d.nodeId = "n5";
        d.category = "亮斑";
        d.centerX = 30.5;
        d.left = 22;
        d.width = 17;
        d.area = 201;
        f.result.defects = {d, d};
    }
    return f;
}

quality::InspectionRecord record(std::int64_t timeMs, std::uint64_t counter, const std::string& verdict) {
    return quality::InspectionRecord::create(fields(timeMs, counter, verdict)).value();
}

Image grayImage(int value = 80) { return imaging::fromMat(cv::Mat(60, 80, CV_8UC1, cv::Scalar(value))).value(); }

}  // namespace

// ---------------- 1. 领域层 ----------------

TEST(InspectionRecord, Q2_MustSayWhoWhenAndWithWhat) {
    using quality::InspectionRecord;
    EXPECT_TRUE(InspectionRecord::create(fields(kNoon, 0, "OK")).ok());
    const auto broken = [](const std::function<void(InspectionRecord::Fields&)>& change) {
        auto f = fields(kNoon, 0, "OK");
        change(f);
        const auto r = InspectionRecord::create(std::move(f));
        return !r.ok() && r.error().code == "quality.record_invalid";
    };
    EXPECT_TRUE(broken([](auto& f) { f.id.clear(); }));
    EXPECT_TRUE(broken([](auto& f) { f.stationId.clear(); }));
    EXPECT_TRUE(broken([](auto& f) { f.timeMs = 0; }));
    EXPECT_TRUE(broken([](auto& f) { f.releaseId.clear(); }));
    EXPECT_TRUE(broken([](auto& f) { f.recipeVersionId.clear(); }));
}

TEST(InspectionRecord, Q3_VerdictIsOneOfThreeAndNgNeedsAReason) {
    using quality::InspectionRecord;
    auto f = fields(kNoon, 0, "MAYBE");
    EXPECT_FALSE(InspectionRecord::create(f).ok());
    f = fields(kNoon, 0, "NG");
    EXPECT_TRUE(InspectionRecord::create(f).ok());
    f.reasons[0].pass = true;  // 只有合格的依据，却判了 NG
    EXPECT_FALSE(InspectionRecord::create(f).ok());
    f.reasons.clear();
    EXPECT_FALSE(InspectionRecord::create(f).ok());
    EXPECT_TRUE(InspectionRecord::create(fields(kNoon, 0, "ERROR")).ok());  // ERROR 不要求有依据
}

TEST(InspectionRecord, IdsSortByTimeThenCounter) {
    EXPECT_EQ(quality::makeRecordId(kNoon, 42), "1795521600000-000042");
    EXPECT_LT(quality::makeRecordId(kNoon, 9), quality::makeRecordId(kNoon, 10));
    EXPECT_LT(quality::makeRecordId(kNoon, 999), quality::makeRecordId(kNoon + 1, 0));
    EXPECT_LT(quality::makeRecordId(999, 0), quality::makeRecordId(kNoon, 0));  // 位数补齐了，短的时间不会排到后面
}

TEST(InspectionRecord, SummaryCountsVerdictsAndDefectsAndLeavesErrorsOutOfYield) {
    quality::Summary empty;
    EXPECT_FALSE(empty.yield().has_value());  // 没有可算的：不是 0%，也不是 100%
    const auto s = quality::summarize({record(kNoon, 0, "OK"), record(kNoon, 1, "OK"), record(kNoon, 2, "OK"), record(kNoon, 3, "NG"),
                                       record(kNoon, 4, "ERROR")});
    EXPECT_EQ(s.total, 5);
    EXPECT_EQ(s.ok, 3);
    EXPECT_EQ(s.ng, 1);
    EXPECT_EQ(s.error, 1);
    EXPECT_DOUBLE_EQ(*s.yield(), 0.75);  // 3 ÷ (3 + 1)：出错的那一次不算
    EXPECT_EQ(s.defectsByCategory.at("亮斑"), 2);
    const auto onlyErrors = quality::summarize({record(kNoon, 0, "ERROR")});
    EXPECT_FALSE(onlyErrors.yield().has_value());
}

// ---------------- 2. 存储 ----------------

TEST(InspectionRecordFiles, OneFilePerStationPerDayOneLinePerRecord) {
    const fs::path dir = freshDir("数据");
    {
        persistence::JsonlInspectionRecordRepository repo(dir);
        ASSERT_TRUE(repo.append(record(kNoon - kDay, 0, "OK")).ok());  // 昨天
        ASSERT_TRUE(repo.append(record(kNoon, 0, "OK")).ok());
        ASSERT_TRUE(repo.append(record(kNoon, 1, "NG")).ok());
        ASSERT_TRUE(repo.append(record(kNoon + 5, 0, "ERROR")).ok());
        auto other = fields(kNoon, 0, "OK");
        other.stationId = "s-2";
        ASSERT_TRUE(repo.append(quality::InspectionRecord::create(other).value()).ok());
    }
    const fs::path today = dir / "inspection-records" / "s-1" / "2026-11-24.jsonl";
    ASSERT_TRUE(fs::exists(today));
    EXPECT_TRUE(fs::exists(dir / "inspection-records" / "s-1" / "2026-11-23.jsonl"));
    EXPECT_TRUE(fs::exists(dir / "inspection-records" / "s-2" / "2026-11-24.jsonl"));
    int lines = 0;
    {
        // 读完就关上：Windows 上，文件还开着的话后面删不掉这个目录
        std::ifstream in(today);
        for (std::string line; std::getline(in, line);) ++lines;
    }
    EXPECT_EQ(lines, 3);

    // 换一个对象来读（相当于程序重启了）：从新到旧，内容一样不少
    persistence::JsonlInspectionRecordRepository repo(dir);
    quality::RecordFilter filter;
    auto found = repo.query("s-1", filter);
    ASSERT_EQ(found.size(), 4u);
    EXPECT_EQ(found[0]->verdict, "ERROR");
    EXPECT_EQ(found[1]->verdict, "NG");
    EXPECT_EQ(found[3]->timeMs, kNoon - kDay);
    const auto& ng = found[1];
    EXPECT_EQ(ng->frameName, "图像.png");
    EXPECT_EQ(ng->releaseId, "rel-1");
    ASSERT_EQ(ng->reasons.size(), 1u);
    EXPECT_EQ(ng->reasons[0].text, "亮斑 2 个，超过上限 0");
    ASSERT_EQ(ng->result.defects.size(), 2u);
    EXPECT_EQ(ng->result.defects[0].category, "亮斑");
    EXPECT_DOUBLE_EQ(ng->result.defects[0].centerX, 30.5);
    EXPECT_EQ(ng->result.defects[0].width, 17);

    // 筛选、翻页、条数
    filter.verdict = "OK";
    EXPECT_EQ(repo.query("s-1", filter).size(), 2u);  // 跨了两天的文件
    filter = {};
    filter.limit = 2;
    found = repo.query("s-1", filter);
    ASSERT_EQ(found.size(), 2u);
    filter.beforeId = found[1]->id;
    found = repo.query("s-1", filter);
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found[0]->id, quality::makeRecordId(kNoon, 0));
    filter.beforeId = found[1]->id;
    EXPECT_TRUE(repo.query("s-1", filter).empty());  // 翻到头了

    // 按编号找
    ASSERT_TRUE(repo.find("s-1", quality::makeRecordId(kNoon, 1)).has_value());
    EXPECT_EQ((*repo.find("s-1", quality::makeRecordId(kNoon - kDay, 0)))->verdict, "OK");
    EXPECT_FALSE(repo.find("s-1", quality::makeRecordId(kNoon, 7)).has_value());
    EXPECT_FALSE(repo.find("s-1", "不是编号").has_value());
    EXPECT_FALSE(repo.find("s-2", quality::makeRecordId(kNoon, 1)).has_value());  // 别的工位的看不到
    EXPECT_EQ(repo.all("s-1").size(), 4u);
    EXPECT_TRUE(repo.all("nobody").empty());
    EXPECT_TRUE(repo.query("../s-1", {}).empty());  // 工位 ID 不能用来走到别的目录去
    fs::remove_all(dir);
}

// 写到一半断电：最后一行是残缺的。别的记录不受影响，之后还能接着写
TEST(InspectionRecordFiles, ATornLastLineLosesOnlyThatRecord) {
    const fs::path dir = freshDir("数据");
    persistence::JsonlInspectionRecordRepository repo(dir);
    ASSERT_TRUE(repo.append(record(kNoon, 0, "OK")).ok());
    ASSERT_TRUE(repo.append(record(kNoon, 1, "NG")).ok());
    const fs::path file = dir / "inspection-records" / "s-1" / "2026-11-24.jsonl";
    { std::ofstream(file, std::ios::binary | std::ios::app) << "{\"id\":\"1795521600000-000002\",\"station\":\"s-1\",\"ti"; }
    EXPECT_EQ(repo.all("s-1").size(), 2u);
    // 残缺的那一行没有换行符。接着写的下一条会和它连在一起——所以追加之前要先补一个换行
    ASSERT_TRUE(repo.append(record(kNoon, 3, "OK")).ok());
    const auto all = repo.query("s-1", {});
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0]->id, quality::makeRecordId(kNoon, 3));
    fs::remove_all(dir);
}

// ---------------- 3. 应用服务 ----------------

namespace {

class FakeClock final : public quality::Clock {
public:
    std::int64_t now = kNoon;
    std::int64_t nowMs() override { return now; }
};

// 放在内存里的图像存档：记着每幅图像是长期的还是在短期缓存里
class FakeArchive final : public quality::ImageArchive {
public:
    bool broken = false;
    std::map<std::string, std::string> images;  // 图像的编号 → kept / cached
    Result<std::string> store(const std::string&, const std::string& recordId, const Image&, bool longTerm) override {
        if (broken) return Result<std::string>::failure("content.write", "磁盘满了");
        images[recordId] = longTerm ? "kept" : "cached";
        return recordId;
    }
    std::string state(const std::string&, const std::string& imageId) override {
        const auto it = images.find(imageId);
        return it == images.end() ? "expired" : it->second;
    }
    Result<std::vector<unsigned char>> load(const std::string&, const std::string& imageId) override {
        if (images.count(imageId) == 0) return Result<std::vector<unsigned char>>::failure("content.not_found", "没有了");
        return std::vector<unsigned char>{'P', 'N', 'G'};
    }
    Status keep(const std::string&, const std::string& imageId) override {
        if (const auto it = images.find(imageId); it != images.end()) it->second = "kept";
        return Status::success();
    }
    quality::ImageCleanup cleanup(std::int64_t cutoffMs, std::uint64_t) override {
        quality::ImageCleanup done;
        for (auto it = images.begin(); it != images.end();) {
            if (it->second == "cached" && std::stoll(it->first.substr(0, 13)) < cutoffMs) {
                it = images.erase(it);
                ++done.expired;
            } else {
                ++it;
            }
        }
        return done;
    }
};

quality::InspectionToRecord inspection(const std::string& verdict) {
    const auto f = fields(kNoon, 0, verdict);
    quality::InspectionToRecord in;
    in.stationId = f.stationId;
    in.sequence = 1;
    in.releaseId = f.releaseId;
    in.recipeVersionId = f.recipeVersionId;
    in.frameName = f.frameName;
    in.verdict = verdict;
    for (const auto& r : f.reasons) in.reasons.push_back({r.nodeId, r.pass, r.text});
    in.result = f.result;
    in.frame = grayImage();
    return in;
}

}  // namespace

TEST(QualityService, RecordIdsAlwaysGoUpEvenWhenTheClockGoesBack) {
    const fs::path dir = freshDir("数据");
    persistence::JsonlInspectionRecordRepository repo(dir);
    FakeClock clock;
    FakeArchive archive;
    quality::QualityService quality(repo, clock, archive);
    std::vector<std::string> ids;
    ids.push_back(quality.record(inspection("OK")).value());
    ids.push_back(quality.record(inspection("OK")).value());  // 同一毫秒
    clock.now += 10;
    ids.push_back(quality.record(inspection("OK")).value());
    clock.now -= 5000;  // 系统对时，时钟往回走了 5 秒
    ids.push_back(quality.record(inspection("OK")).value());
    ids.push_back(quality.record(inspection("OK")).value());
    for (std::size_t i = 1; i < ids.size(); ++i) EXPECT_LT(ids[i - 1], ids[i]) << i;

    const auto records = quality.query("s-1");
    ASSERT_EQ(records.size(), 5u);
    EXPECT_EQ(records[0].id, ids[4]);  // 从新到旧
    EXPECT_EQ(records[4].time, "2026-11-24 12:00:00");
    EXPECT_EQ(quality.getRecord("s-1", ids[2]).value().timeMs, kNoon + 10);
    EXPECT_EQ(quality.getRecord("s-1", "1795521600000-999999").error().code, "quality.record_not_found");
    // 条数的上下限
    quality::RecordQuery q;
    q.limit = 0;
    EXPECT_EQ(quality.query("s-1", q).size(), 1u);
    fs::remove_all(dir);
}

TEST(QualityService, KeepsImagesOnlyForWhatThePolicySays) {
    const auto imagesKept = [](quality::ImagePolicy policy) {
        const fs::path dir = freshDir("数据");
        persistence::JsonlInspectionRecordRepository repo(dir);
        FakeClock clock;
        FakeArchive archive;
        quality::QualityService quality(repo, clock, archive, policy);
        for (const char* verdict : {"OK", "NG", "ERROR", "OK"}) EXPECT_TRUE(quality.record(inspection(verdict)).ok());
        std::string kept;
        for (const auto& r : quality.query("s-1")) kept = r.imageState + (" " + kept);
        fs::remove_all(dir);
        return kept;
    };
    // 依次是判 OK、NG、ERROR、OK 的四次检测
    EXPECT_EQ(imagesKept(quality::ImagePolicy::All), "cached kept kept cached ");  // M1、M2
    EXPECT_EQ(imagesKept(quality::ImagePolicy::NgAndError), "none kept kept none ");
    EXPECT_EQ(imagesKept(quality::ImagePolicy::None), "none none none none ");
}

TEST(QualityService, ARecordIsStillWrittenWhenItsImageCannotBeStored) {
    const fs::path dir = freshDir("数据");
    persistence::JsonlInspectionRecordRepository repo(dir);
    FakeClock clock;
    FakeArchive archive;
    archive.broken = true;
    quality::QualityService quality(repo, clock, archive);
    ASSERT_TRUE(quality.record(inspection("NG")).ok());
    auto noFrame = inspection("NG");
    noFrame.frame = Image();  // 没有图像可留
    ASSERT_TRUE(quality.record(noFrame).ok());
    const auto records = quality.query("s-1");
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].imageState, "none");
    EXPECT_EQ(records[1].imageState, "none");
    // 说不清用什么检测的（Q2）：不记，并且说明原因
    auto bad = inspection("OK");
    bad.releaseId.clear();
    EXPECT_EQ(quality.record(bad).error().code, "quality.record_invalid");
    EXPECT_EQ(quality.query("s-1").size(), 2u);
    fs::remove_all(dir);
}

// 汇总第一次问到时读一遍磁盘，之后靠累加。两种算法要对得上
TEST(QualityService, SummaryStaysRightAsRecordsKeepComing) {
    const fs::path dir = freshDir("数据");
    persistence::JsonlInspectionRecordRepository repo(dir);
    FakeClock clock;
    FakeArchive archive;
    {
        quality::QualityService before(repo, clock, archive);
        ASSERT_TRUE(before.record(inspection("OK")).ok());
        ASSERT_TRUE(before.record(inspection("NG")).ok());
    }
    quality::QualityService quality(repo, clock, archive);  // 程序重启了
    auto s = quality.summary("s-1");
    EXPECT_EQ(s.total, 2);
    EXPECT_DOUBLE_EQ(*s.yield, 0.5);
    ASSERT_TRUE(quality.record(inspection("OK")).ok());
    ASSERT_TRUE(quality.record(inspection("OK")).ok());
    ASSERT_TRUE(quality.record(inspection("ERROR")).ok());
    s = quality.summary("s-1");
    EXPECT_EQ(s.total, 5);
    EXPECT_EQ(s.ok, 3);
    EXPECT_EQ(s.error, 1);
    EXPECT_DOUBLE_EQ(*s.yield, 0.75);
    EXPECT_EQ(s.defectsByCategory.at("亮斑"), 2);
    EXPECT_EQ(quality.summary("s-9").total, 0);
    EXPECT_FALSE(quality.summary("s-9").yield.has_value());
    fs::remove_all(dir);
}

TEST(QualityService, SeveralStationsRecordAtTheSameTime) {
    const fs::path dir = freshDir("数据");
    persistence::JsonlInspectionRecordRepository repo(dir);
    FakeClock clock;  // 时间不动：所有记录都落在同一毫秒，编号全靠序号分开
    FakeArchive archive;
    quality::QualityService quality(repo, clock, archive, quality::ImagePolicy::None);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            auto in = inspection(t % 2 == 0 ? "OK" : "NG");
            in.stationId = "s-" + std::to_string(t % 2);
            for (int i = 0; i < 50; ++i) {
                EXPECT_TRUE(quality.record(in).ok());
                (void)quality.summary(in.stationId);
                (void)quality.query(in.stationId);
            }
        });
    }
    for (auto& t : threads) t.join();
    quality::RecordQuery q;
    q.limit = 500;
    for (const char* station : {"s-0", "s-1"}) {
        const auto records = quality.query(station, q);
        ASSERT_EQ(records.size(), 100u);
        for (std::size_t i = 1; i < records.size(); ++i) EXPECT_GT(records[i - 1].id, records[i].id);  // 没有重复的编号
        EXPECT_EQ(quality.summary(station).total, 100);
    }
    fs::remove_all(dir);
}

// ---------------- 4. 整条路 ----------------

namespace {

Image spots(int count) {
    cv::Mat m(120, 160, CV_8UC1, cv::Scalar(80));
    for (int i = 0; i < count; ++i) cv::circle(m, {30 + 40 * i, 60}, 8, cv::Scalar(240), cv::FILLED);
    return imaging::fromMat(m).value();
}

}  // namespace

TEST(InspectionRecordsEndToEnd, EveryStationInspectionLeavesARecordThatSurvivesARestart) {
    const fs::path data = freshDir("数据");
    std::string station, release, version;
    {
        platform::Platform p(data);
        p.stations.allowUnacceptedReleases(true);  // 这里测的不是验收：允许部署没验收的上线包（规则 L4 的测试在 acceptance_test.cpp、release_bundle_test.cpp）
        auto& a = p.authoring;
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
        version = a.publish(recipe).value();
        release = p.releases.createRelease(version).value();
        station = p.stations.createStation("一号").value();
        ASSERT_TRUE(p.stations.deploy(station, release).ok());
        ASSERT_TRUE(p.stations.waitForLoading(station).ok());
        ASSERT_TRUE(p.stations.start(station).ok());

        ASSERT_TRUE(p.stations.inspect(station, spots(0), "好的.png").ok());
        const auto ng = p.stations.inspect(station, spots(2), "坏的.png");
        ASSERT_TRUE(ng.ok());
        EXPECT_EQ(ng.value().frameName, "坏的.png");
        ASSERT_TRUE(p.stations.inspect(station, spots(0)).ok());  // 没有名字也可以
        // 调试运行、试运行不是产线检测：不留记录
        ASSERT_TRUE(p.execution.debugRun(p.authoring.getVersionDocument(version).value(), spots(2)).ok());
        ASSERT_TRUE(p.stations.stop(station).ok());
        // 工位没有在运行：检测被拒绝，也不留记录
        EXPECT_FALSE(p.stations.inspect(station, spots(2)).ok());
    }

    platform::LinePlatform p(data);  // 程序重启了；而且换成了产线运行平台
    const auto records = p.quality.query(station);
    ASSERT_EQ(records.size(), 3u);
    const auto& ng = records[1];
    EXPECT_EQ(ng.verdict, "NG");
    EXPECT_EQ(ng.frameName, "坏的.png");
    EXPECT_EQ(ng.sequence, 2u);
    EXPECT_EQ(ng.releaseId, release);
    EXPECT_EQ(ng.recipeVersionId, version);  // 由它可以追到当时的每一个参数
    EXPECT_EQ(ng.result.defects.size(), 2u);
    EXPECT_GT(ng.totalMs, 0);
    ASSERT_FALSE(ng.reasons.empty());
    // 不合格的那一次，图像留下来了，取出来就是当时的那一幅
    EXPECT_EQ(ng.imageState, "kept");
    const auto bytes = p.quality.recordImage(station, ng.id);
    ASSERT_TRUE(bytes.ok());
    const cv::Mat saved = cv::imdecode(bytes.value(), cv::IMREAD_UNCHANGED);
    ASSERT_EQ(saved.cols, 160);
    EXPECT_EQ(saved.at<unsigned char>(60, 30), 240);
    // 合格的进短期缓存：现在还看得到，过了保留期会清掉
    EXPECT_EQ(records[0].imageState, "cached");
    EXPECT_EQ(records[2].imageState, "cached");
    EXPECT_TRUE(p.quality.recordImage(station, records[0].id).ok());
    EXPECT_TRUE(records[0].frameName.empty());

    const auto s = p.quality.summary(station);
    EXPECT_EQ(s.total, 3);
    EXPECT_NEAR(*s.yield, 2.0 / 3, 1e-9);
    EXPECT_EQ(s.defectsByCategory.size(), 1u);
    fs::remove_all(data);
}

// =====================================================================================
// 复判（QUA-01，docs/ddd/17-review.md）
// =====================================================================================

#include "vision/quality/domain/review.h"

namespace {

quality::Review review(const std::string& recordId, const std::string& verdict, bool truthNg, std::int64_t timeMs = kNoon) {
    quality::Review::Fields f;
    f.recordId = recordId;
    f.stationId = "s-1";
    f.verdict = verdict;
    f.truthNg = truthNg;
    f.timeMs = timeMs;
    return quality::Review::create(std::move(f)).value();
}

}  // namespace

// ---------------- 领域层 ----------------

TEST(Review, R4_TheConclusionComesFromVerdictAndTruth) {
    using quality::ReviewKind;
    EXPECT_EQ(review("r", "NG", false).kind(), ReviewKind::FalseAlarm);  // 判了不合格，实际合格：误报
    EXPECT_EQ(review("r", "OK", true).kind(), ReviewKind::Miss);         // 判了合格，实际不合格：漏检
    EXPECT_EQ(review("r", "OK", false).kind(), ReviewKind::Correct);
    EXPECT_EQ(review("r", "NG", true).kind(), ReviewKind::Correct);
    EXPECT_EQ(review("r", "ERROR", true).kind(), ReviewKind::WasError);  // 系统没检测下来：不算误报也不算漏检
    EXPECT_EQ(review("r", "ERROR", false).kind(), ReviewKind::WasError);
    EXPECT_TRUE(review("r", "NG", false).misjudged());
    EXPECT_TRUE(review("r", "OK", true).misjudged());
    EXPECT_FALSE(review("r", "OK", false).misjudged());
    EXPECT_FALSE(review("r", "ERROR", true).misjudged());
    EXPECT_STREQ(quality::reviewKindName(ReviewKind::FalseAlarm), "false_alarm");
    EXPECT_STREQ(quality::reviewKindName(ReviewKind::Miss), "miss");
}

TEST(Review, MustPointAtARecordAndCarryAKnownVerdict) {
    const auto broken = [](const std::function<void(quality::Review::Fields&)>& change) {
        quality::Review::Fields f;
        f.recordId = "r";
        f.stationId = "s-1";
        f.verdict = "NG";
        f.timeMs = kNoon;
        change(f);
        const auto r = quality::Review::create(std::move(f));
        return !r.ok() && r.error().code == "quality.review_invalid";
    };
    EXPECT_FALSE(broken([](auto&) {}));
    EXPECT_TRUE(broken([](auto& f) { f.recordId.clear(); }));
    EXPECT_TRUE(broken([](auto& f) { f.stationId.clear(); }));
    EXPECT_TRUE(broken([](auto& f) { f.verdict = "MAYBE"; }));
    EXPECT_TRUE(broken([](auto& f) { f.timeMs = 0; }));
}

TEST(Review, R3_OnlyTheLatestReviewOfARecordCounts) {
    quality::ReviewBook book;
    EXPECT_EQ(book.latest("a"), nullptr);
    book.add(review("a", "NG", false));  // 误报
    book.add(review("b", "OK", true));   // 漏检
    book.add(review("c", "OK", false));  // 判对了
    book.add(review("d", "ERROR", true));
    auto s = book.summary();
    EXPECT_EQ(s.reviewed, 4);
    EXPECT_EQ(s.falseAlarms, 1);
    EXPECT_EQ(s.misses, 1);
    EXPECT_EQ(s.correct, 1);
    EXPECT_EQ(s.wasError, 1);
    // 改主意：a 其实是不合格的（系统判对了）；b 其实是合格的
    book.add(review("a", "NG", true, kNoon + 1));
    book.add(review("b", "OK", false, kNoon + 2));
    s = book.summary();
    EXPECT_EQ(s.reviewed, 4);  // 还是 4 条记录
    EXPECT_EQ(s.falseAlarms, 0);
    EXPECT_EQ(s.misses, 0);
    EXPECT_EQ(s.correct, 3);
    EXPECT_TRUE(book.latest("a")->fields().truthNg);
    EXPECT_EQ(book.latest("a")->fields().timeMs, kNoon + 1);
}

// ---------------- 存储 ----------------

TEST(ReviewFiles, AppendOnlyNextToTheRecordsWithoutBeingMistakenForThem) {
    const fs::path dir = freshDir("数据");
    persistence::JsonlInspectionRecordRepository records(dir);
    persistence::JsonlReviewRepository reviews(dir);
    ASSERT_TRUE(records.append(record(kNoon, 0, "NG")).ok());
    auto first = review(quality::makeRecordId(kNoon, 0), "NG", false, kNoon + 5);
    ASSERT_TRUE(reviews.append(first).ok());
    quality::Review::Fields f = first.fields();
    f.truthNg = true;
    f.reviewer = "小李";
    f.note = "确实有\"划痕\"\n第二行";  // 引号、换行都要原样回来
    f.timeMs = kNoon + 9;
    ASSERT_TRUE(reviews.append(quality::Review::create(f).value()).ok());
    EXPECT_TRUE(fs::exists(dir / "inspection-records" / "s-1" / "reviews.jsonl"));

    persistence::JsonlReviewRepository reopened(dir);
    const auto all = reopened.all("s-1");
    ASSERT_EQ(all.size(), 2u);  // 两次都在（R3：之前那次留着）
    EXPECT_FALSE(all[0]->truthNg);
    EXPECT_TRUE(all[1]->truthNg);
    EXPECT_EQ(all[1]->reviewer, "小李");
    EXPECT_EQ(all[1]->note, "确实有\"划痕\"\n第二行");
    EXPECT_EQ(all[1]->verdict, "NG");
    EXPECT_TRUE(reopened.all("nobody").empty());
    EXPECT_TRUE(reopened.all("../s-1").empty());
    // 复判的文件和检测记录在同一个目录里：读检测记录时不能把它当成记录
    EXPECT_EQ(records.all("s-1").size(), 1u);
    EXPECT_EQ(records.query("s-1", {}).size(), 1u);
    // 残缺的最后一行只丢那一条
    { std::ofstream(dir / "inspection-records" / "s-1" / "reviews.jsonl", std::ios::binary | std::ios::app) << "{\"record\":\"x\",\"sta"; }
    EXPECT_EQ(reopened.all("s-1").size(), 2u);
    ASSERT_TRUE(reopened.append(first).ok());
    EXPECT_EQ(reopened.all("s-1").size(), 3u);
    fs::remove_all(dir);
}

// ---------------- 应用服务 ----------------

namespace {

struct ReviewFixture {
    explicit ReviewFixture(const fs::path& dir) : records(dir), reviews(dir), quality(records, clock, archive) {
        quality.useReviews(reviews, bus);
        bus.subscribe("MisjudgmentConfirmed", [this](const EventPtr& e) {
            const auto& m = static_cast<const quality::MisjudgmentConfirmed&>(*e);
            confirmed.push_back(m.kind + " " + m.recordId + " " + m.imageId);
        });
    }
    std::string add(const std::string& verdict) {
        clock.now += 10;
        return quality.record(inspection(verdict)).value();
    }
    persistence::JsonlInspectionRecordRepository records;
    persistence::JsonlReviewRepository reviews;
    FakeClock clock;
    FakeArchive archive;
    eventbus::InMemoryEventBus bus;
    quality::QualityService quality;
    std::vector<std::string> confirmed;
};

}  // namespace

TEST(QualityReview, ReviewingARecordTellsFalseAlarmsFromMisses) {
    const fs::path dir = freshDir("数据");
    ReviewFixture f(dir);
    const std::string ng = f.add("NG"), ok = f.add("OK"), error = f.add("ERROR");
    EXPECT_FALSE(f.quality.getRecord("s-1", ng).value().review.has_value());  // 还没复判

    // 判了 NG，人确认其实是合格的：误报
    auto r = f.quality.review("s-1", ng, {false, "小李", "是反光"});
    ASSERT_TRUE(r.ok()) << r.error().message;
    ASSERT_TRUE(r.value().review.has_value());
    EXPECT_EQ(r.value().review->kind, "false_alarm");
    EXPECT_FALSE(r.value().review->truthNg);
    EXPECT_EQ(r.value().review->reviewer, "小李");
    EXPECT_EQ(r.value().review->note, "是反光");
    EXPECT_EQ(r.value().review->time.size(), 19u);
    EXPECT_EQ(r.value().verdict, "NG");  // 检测记录本身没有变（Q1）
    // 判了 OK，其实不合格：漏检
    EXPECT_EQ(f.quality.review("s-1", ok, {true, "", ""}).value().review->kind, "miss");
    // 系统当时出错：人补上真实情况，不算误判
    EXPECT_EQ(f.quality.review("s-1", error, {true, "", ""}).value().review->kind, "was_error");

    // 误判各发一个事件，带着当时的图像
    ASSERT_EQ(f.confirmed.size(), 2u);
    EXPECT_EQ(f.confirmed[0], "false_alarm " + ng + " " + ng);  // 图像的编号就是记录的编号
    EXPECT_EQ(f.confirmed[1], "miss " + ok + " " + ok);

    const auto s = f.quality.summary("s-1");
    EXPECT_EQ(s.total, 3);
    EXPECT_EQ(s.ng, 1);  // 系统的统计不因为复判而改
    EXPECT_EQ(s.reviewed, 3);
    EXPECT_EQ(s.falseAlarms, 1);
    EXPECT_EQ(s.misses, 1);
    EXPECT_EQ(s.reviewedErrors, 1);
    EXPECT_EQ(s.reviewedCorrect, 0);

    // R1：记录必须存在；别的工位的记录不行
    EXPECT_EQ(f.quality.review("s-1", "1795521600000-999999", {false, "", ""}).error().code, "quality.record_not_found");
    EXPECT_EQ(f.quality.review("s-2", ng, {false, "", ""}).error().code, "quality.record_not_found");
    EXPECT_EQ(f.quality.summary("s-1").reviewed, 3);
    fs::remove_all(dir);
}

TEST(QualityReview, R3_ChangingYourMindKeepsHistoryAndTheLatestWins) {
    const fs::path dir = freshDir("数据");
    ReviewFixture f(dir);
    const std::string ng = f.add("NG");
    ASSERT_TRUE(f.quality.review("s-1", ng, {false, "", "看着像反光"}).ok());  // 误报
    f.clock.now += 1000;
    const auto again = f.quality.review("s-1", ng, {true, "", "拿到零件看了：真有划痕"});
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again.value().review->kind, "correct");
    EXPECT_EQ(again.value().review->note, "拿到零件看了：真有划痕");
    auto s = f.quality.summary("s-1");
    EXPECT_EQ(s.reviewed, 1);
    EXPECT_EQ(s.falseAlarms, 0);
    EXPECT_EQ(s.reviewedCorrect, 1);
    EXPECT_EQ(f.reviews.all("s-1").size(), 2u);  // 两次都留着
    EXPECT_EQ(f.confirmed.size(), 1u);           // 第二次不是误判，不再发事件

    // 程序重启之后，结论还是最近的那一次
    ReviewFixture restarted(dir);
    EXPECT_EQ(restarted.quality.getRecord("s-1", ng).value().review->kind, "correct");
    s = restarted.quality.summary("s-1");
    EXPECT_EQ(s.reviewed, 1);
    EXPECT_EQ(s.reviewedCorrect, 1);
    // 重启后接着复判，不会把磁盘上已有的再算一遍
    ASSERT_TRUE(restarted.quality.review("s-1", ng, {false, "", ""}).ok());
    s = restarted.quality.summary("s-1");
    EXPECT_EQ(s.reviewed, 1);
    EXPECT_EQ(s.falseAlarms, 1);
    EXPECT_EQ(s.reviewedCorrect, 0);
    fs::remove_all(dir);
}

TEST(QualityReview, FilterByReviewStateAcrossPages) {
    const fs::path dir = freshDir("数据");
    ReviewFixture f(dir);
    std::vector<std::string> ids;
    for (int i = 0; i < 40; ++i) ids.push_back(f.add(i % 2 == 0 ? "NG" : "OK"));
    // 最早的 3 条和最新的 1 条：误判；另外复判对了 2 条
    for (const std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{39}}) {
        ASSERT_TRUE(f.quality.review("s-1", ids[i], {i % 2 != 0, "", ""}).ok());  // NG 的说合格、OK 的说不合格
    }
    ASSERT_TRUE(f.quality.review("s-1", ids[10], {true, "", ""}).ok());   // NG、确实不合格
    ASSERT_TRUE(f.quality.review("s-1", ids[11], {false, "", ""}).ok());  // OK、确实合格

    quality::RecordQuery q;
    q.review = "misjudged";
    q.limit = 3;  // 一页 3 条：误判的散在最新和最早的两头，中间隔着三十多条——要翻好几页才凑得齐
    auto page = f.quality.query("s-1", q);
    ASSERT_EQ(page.size(), 3u);
    EXPECT_EQ(page[0].id, ids[39]);
    EXPECT_EQ(page[1].id, ids[2]);
    EXPECT_EQ(page[2].id, ids[1]);
    q.beforeId = page.back().id;
    page = f.quality.query("s-1", q);
    ASSERT_EQ(page.size(), 1u);
    EXPECT_EQ(page[0].id, ids[0]);

    q = {};
    q.review = "unreviewed";
    q.limit = 500;
    EXPECT_EQ(f.quality.query("s-1", q).size(), 34u);
    q.verdict = "NG";  // 两个条件一起：还没复判的、判 NG 的
    const auto ngUnreviewed = f.quality.query("s-1", q);
    EXPECT_EQ(ngUnreviewed.size(), 17u);
    for (const auto& r : ngUnreviewed) EXPECT_FALSE(r.review.has_value());
    // 不按复判筛选时，每条记录上带着它的复判
    q = {};
    q.limit = 500;
    int reviewed = 0;
    for (const auto& r : f.quality.query("s-1", q)) reviewed += r.review ? 1 : 0;
    EXPECT_EQ(reviewed, 6);
    fs::remove_all(dir);
}

TEST(QualityReview, WithoutReviewStorageItSaysSo) {
    const fs::path dir = freshDir("数据");
    persistence::JsonlInspectionRecordRepository records(dir);
    FakeClock clock;
    FakeArchive archive;
    quality::QualityService quality(records, clock, archive);
    const std::string id = quality.record(inspection("NG")).value();
    EXPECT_EQ(quality.review("s-1", id, {false, "", ""}).error().code, "quality.no_reviews");
    EXPECT_EQ(quality.summary("s-1").reviewed, 0);
    EXPECT_EQ(quality.query("s-1").size(), 1u);
    fs::remove_all(dir);
}

// ---------------- 整条路：复判 → 导出成两个目录 → 建基准图集 ----------------

TEST(ReviewEndToEnd, ReviewedImagesBecomeAGoldenSet) {
    const fs::path data = freshDir("数据"), out = freshDir("导出");
    platform::Platform p(data);
    p.stations.allowUnacceptedReleases(true);
    auto& a = p.authoring;
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
    const std::string release = p.releases.createRelease(a.publish(recipe).value()).value();
    const std::string station = p.stations.createStation("一号").value();
    ASSERT_TRUE(p.stations.deploy(station, release).ok());
    ASSERT_TRUE(p.stations.waitForLoading(station).ok());
    ASSERT_TRUE(p.stations.start(station).ok());
    for (const int count : {0, 1, 2, 3}) ASSERT_TRUE(p.stations.inspect(station, spots(count)).ok());
    ASSERT_TRUE(p.stations.stop(station).ok());

    quality::RecordQuery all;
    const auto records = p.quality.query(station, all);  // 从新到旧：3 个亮斑、2、1、0
    ASSERT_EQ(records.size(), 4u);
    for (const auto& r : records) EXPECT_FALSE(r.imageId.empty());
    // 没复判过：什么都导不出来
    auto exported = platform::exportReviewedImages(p, station, pathToUtf8(out), false);
    ASSERT_TRUE(exported.ok());
    EXPECT_EQ(exported.value().ok + exported.value().ng, 0);

    ASSERT_TRUE(p.quality.review(station, records[0].id, {true, "", ""}).ok());   // NG，确实不合格
    ASSERT_TRUE(p.quality.review(station, records[2].id, {false, "", ""}).ok());  // NG（1 个亮斑），其实可以放行：误报
    ASSERT_TRUE(p.quality.review(station, records[3].id, {true, "", ""}).ok());   // OK，其实不合格：漏检

    exported = platform::exportReviewedImages(p, station, pathToUtf8(out / pathFromUtf8("全部")), false);
    ASSERT_TRUE(exported.ok()) << exported.error().message;
    EXPECT_EQ(exported.value().ok, 1);
    EXPECT_EQ(exported.value().ng, 2);
    EXPECT_TRUE(fs::exists(out / pathFromUtf8("全部") / "ok" / (records[2].id + ".png")));
    EXPECT_TRUE(fs::exists(out / pathFromUtf8("全部") / "ng" / (records[3].id + ".png")));
    // 只要误判的
    exported = platform::exportReviewedImages(p, station, pathToUtf8(out / "misjudged"), true);
    ASSERT_TRUE(exported.ok());
    EXPECT_EQ(exported.value().ok, 1);
    EXPECT_EQ(exported.value().ng, 1);
    EXPECT_EQ(platform::exportReviewedImages(p, "nobody", pathToUtf8(out), false).error().code, "station.not_found");

    // 导出的两个目录就是建基准图集要的样子。拿它验收当前的方案：正好暴露出那一次误报和那一次漏检
    const auto golden = platform::createGoldenSetFromFolders(p, "复判出来的", pathToUtf8(out / pathFromUtf8("全部") / "ok"),
                                                             pathToUtf8(out / pathFromUtf8("全部") / "ng"), {});
    ASSERT_TRUE(golden.ok()) << golden.error().message;
    const auto acceptance = p.releases.checkAcceptance(release, golden.value());
    ASSERT_TRUE(acceptance.ok()) << acceptance.error().message;
    EXPECT_FALSE(acceptance.value().passed);
    EXPECT_EQ(acceptance.value().falseAlarms, 1);
    EXPECT_EQ(acceptance.value().misses, 1);
    fs::remove_all(data);
    fs::remove_all(out);
}

// 选了“只留不合格的图像”时：复判确认的漏检没有图像可导。导出时要说出来，不能悄悄少一幅
TEST(ReviewEndToEnd, AMissWithoutAKeptImageIsCountedNotSilentlyDropped) {
    const fs::path data = freshDir("数据"), out = freshDir("导出");
    platform::Platform p(data);
    p.stations.allowUnacceptedReleases(true);
    p.quality.setImagePolicy(quality::ImagePolicy::NgAndError);  // 相当于 --keep-images ng
    auto& a = p.authoring;
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
    const std::string release = p.releases.createRelease(a.publish(recipe).value()).value();
    const std::string station = p.stations.createStation("一号").value();
    ASSERT_TRUE(p.stations.deploy(station, release).ok());
    ASSERT_TRUE(p.stations.waitForLoading(station).ok());
    ASSERT_TRUE(p.stations.start(station).ok());
    ASSERT_TRUE(p.stations.inspect(station, spots(0)).ok());  // 判 OK：这个设置下不留图像
    ASSERT_TRUE(p.stations.inspect(station, spots(2)).ok());  // 判 NG：留
    ASSERT_TRUE(p.stations.stop(station).ok());
    const auto records = p.quality.query(station);
    ASSERT_TRUE(p.quality.review(station, records[1].id, {true, "", "下道工序发现的"}).ok());  // 漏检
    ASSERT_TRUE(p.quality.review(station, records[0].id, {true, "", ""}).ok());               // 判对了

    const auto exported = platform::exportReviewedImages(p, station, pathToUtf8(out), false);
    ASSERT_TRUE(exported.ok());
    EXPECT_EQ(exported.value().ng, 1);
    EXPECT_EQ(exported.value().ok, 0);
    EXPECT_EQ(exported.value().withoutImage, 1);  // 那一次漏检：有复判，没有图像
    fs::remove_all(data);
    fs::remove_all(out);
}


// =====================================================================================
// 短期图像缓存（F-3，docs/ddd/18-image-cache.md）
// =====================================================================================

#include "vision/quality/domain/image_retention.h"

// ---------------- 领域层 ----------------

TEST(ImageRetention, M1_M2_WhatIsKeptAndForHowLong) {
    using quality::ImageKeeping;
    quality::ImageRetention r;  // 默认
    EXPECT_EQ(r.at("NG"), ImageKeeping::LongTerm);
    EXPECT_EQ(r.at("ERROR"), ImageKeeping::LongTerm);
    EXPECT_EQ(r.at("OK"), ImageKeeping::ShortTerm);
    EXPECT_EQ(r.retentionMs, 72LL * 3600 * 1000);
    EXPECT_EQ(r.cutoff(kNoon), kNoon - 72LL * 3600 * 1000);
    r.cacheAccepted = false;
    EXPECT_EQ(r.at("OK"), ImageKeeping::Discard);
    EXPECT_EQ(r.at("NG"), ImageKeeping::LongTerm);
    r.keepRejected = false;
    EXPECT_EQ(r.at("NG"), ImageKeeping::Discard);

    EXPECT_EQ(quality::ImageRetention::create(true, true, 24).value().retentionMs, 24LL * 3600 * 1000);
    EXPECT_TRUE(quality::ImageRetention::create(true, true, 1).ok());
    for (const double hours : {0.0, 0.5, -3.0, 24.0 * 400, std::nan("")}) {
        const auto bad = quality::ImageRetention::create(true, true, hours);
        ASSERT_FALSE(bad.ok()) << hours;
        EXPECT_EQ(bad.error().code, "quality.retention_invalid");
    }
}

// ---------------- 应用服务 ----------------

TEST(ImageCache, M2_M4_AcceptedImagesExpireButTheirRecordsStay) {
    const fs::path dir = freshDir("数据");
    ReviewFixture f(dir);
    const std::string ok = f.add("OK"), ng = f.add("NG");
    EXPECT_EQ(f.quality.getRecord("s-1", ok).value().imageState, "cached");
    EXPECT_TRUE(f.quality.recordImage("s-1", ok).ok());

    // 71 小时后：还没到期
    f.clock.now += 71LL * 3600 * 1000;
    EXPECT_EQ(f.quality.cleanExpiredImages().expired, 0);
    EXPECT_EQ(f.quality.getRecord("s-1", ok).value().imageState, "cached");
    // 再过 2 小时：判 OK 的那一幅过期了；判 NG 的还在
    f.clock.now += 2LL * 3600 * 1000;
    EXPECT_EQ(f.quality.cleanExpiredImages().expired, 1);
    const auto record = f.quality.getRecord("s-1", ok);
    ASSERT_TRUE(record.ok());  // M4：记录还在，写着它曾经有图像
    EXPECT_EQ(record.value().imageState, "expired");
    EXPECT_EQ(record.value().verdict, "OK");
    const auto image = f.quality.recordImage("s-1", ok);
    ASSERT_FALSE(image.ok());
    EXPECT_EQ(image.error().code, "quality.image_not_found");
    EXPECT_NE(image.error().message.find("保留期"), std::string::npos);
    EXPECT_EQ(f.quality.getRecord("s-1", ng).value().imageState, "kept");
    EXPECT_TRUE(f.quality.recordImage("s-1", ng).ok());
    EXPECT_EQ(f.quality.summary("s-1").total, 2);
    EXPECT_EQ(f.quality.cleanExpiredImages().expired, 0);  // 再清一次：没有可清的了
    // 过期之后还能复判（操作员从下道工序知道它有问题），只是没有图像
    EXPECT_EQ(f.quality.review("s-1", ok, {true, "", ""}).value().review->kind, "miss");
    EXPECT_EQ(f.quality.recordImage("s-1", "1795521600000-999999").error().code, "quality.record_not_found");
    fs::remove_all(dir);
}

TEST(ImageCache, M3_AReviewedImageIsKeptForGood) {
    const fs::path dir = freshDir("数据");
    ReviewFixture f(dir);
    const std::string reviewed = f.add("OK"), untouched = f.add("OK");
    // 复判——哪怕结论是“系统判对了”——图像就转成长期保留
    const auto r = f.quality.review("s-1", reviewed, {false, "", ""});
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value().review->kind, "correct");
    EXPECT_EQ(r.value().imageState, "kept");
    f.clock.now += 100LL * 3600 * 1000;
    EXPECT_EQ(f.quality.cleanExpiredImages().expired, 1);
    EXPECT_EQ(f.quality.getRecord("s-1", reviewed).value().imageState, "kept");
    EXPECT_EQ(f.quality.getRecord("s-1", untouched).value().imageState, "expired");
    fs::remove_all(dir);
}

TEST(ImageCache, RetentionCanBeChangedWithinLimits) {
    const fs::path dir = freshDir("数据");
    ReviewFixture f(dir);
    const std::string ok = f.add("OK");
    EXPECT_EQ(f.quality.setImageRetentionHours(0.2).error().code, "quality.retention_invalid");
    ASSERT_TRUE(f.quality.setImageRetentionHours(2).ok());
    f.clock.now += 1LL * 3600 * 1000;
    EXPECT_EQ(f.quality.cleanExpiredImages().expired, 0);
    f.clock.now += 2LL * 3600 * 1000;
    EXPECT_EQ(f.quality.cleanExpiredImages().expired, 1);
    EXPECT_EQ(f.quality.getRecord("s-1", ok).value().imageState, "expired");
    // 记录里没有图像的：说的是“当时没留”，不是“过期了”
    f.quality.setImagePolicy(quality::ImagePolicy::NgAndError);
    const std::string none = f.add("OK");
    EXPECT_NE(f.quality.recordImage("s-1", none).error().message.find("当时没有留"), std::string::npos);
    fs::remove_all(dir);
}

// ---------------- 存储：真的文件 ----------------

namespace {

struct ImageFiles {
    explicit ImageFiles(const fs::path& dir) : root(dir), legacy(dir), files(dir, legacy) {}
    fs::path cached(const std::string& station, const std::string& day, const std::string& id) const {
        return root / "inspection-images" / station / "cache" / day / (id + ".png");
    }
    fs::path kept(const std::string& station, const std::string& id) const { return root / "inspection-images" / station / "kept" / (id + ".png"); }
    fs::path root;
    content::FileContentStore legacy;
    content::InspectionImageFiles files;
};

const std::int64_t kHour = 3600 * 1000;

}  // namespace

TEST(InspectionImageFiles, KeptAndCachedLiveInSeparatePlacesByStationAndDay) {
    const fs::path dir = freshDir("数据");
    ImageFiles f(dir);
    const std::string ng = quality::makeRecordId(kNoon, 0), ok = quality::makeRecordId(kNoon, 1);
    EXPECT_EQ(f.files.store("s-1", ng, grayImage(200), true).value(), ng);  // 图像的编号就是记录的编号
    EXPECT_EQ(f.files.store("s-1", ok, grayImage(90), false).value(), ok);
    EXPECT_TRUE(fs::exists(f.kept("s-1", ng)));
    EXPECT_TRUE(fs::exists(f.cached("s-1", "2026-11-24", ok)));
    EXPECT_EQ(f.files.state("s-1", ng), "kept");
    EXPECT_EQ(f.files.state("s-1", ok), "cached");
    EXPECT_EQ(f.files.state("s-1", quality::makeRecordId(kNoon, 9)), "expired");
    EXPECT_EQ(f.files.state("s-2", ok), "expired");  // 别的工位的看不到

    // 取出来就是存进去的那一幅
    const auto bytes = f.files.load("s-1", ok);
    ASSERT_TRUE(bytes.ok());
    const cv::Mat back = cv::imdecode(bytes.value(), cv::IMREAD_UNCHANGED);
    ASSERT_EQ(back.cols, 80);
    EXPECT_EQ(back.at<unsigned char>(10, 10), 90);
    EXPECT_FALSE(f.files.load("s-1", quality::makeRecordId(kNoon, 9)).ok());

    // M3：转成长期保留——文件挪了地方，内容不变；再转一次、转一个不存在的，都不算出错
    ASSERT_TRUE(f.files.keep("s-1", ok).ok());
    EXPECT_EQ(f.files.state("s-1", ok), "kept");
    EXPECT_FALSE(fs::exists(f.cached("s-1", "2026-11-24", ok)));
    EXPECT_EQ(f.files.load("s-1", ok).value(), bytes.value());
    EXPECT_TRUE(f.files.keep("s-1", ok).ok());
    EXPECT_TRUE(f.files.keep("s-1", quality::makeRecordId(kNoon, 9)).ok());

    // 编号不能被用来走到别的目录去
    EXPECT_FALSE(f.files.store("../x", ok, grayImage(), false).ok());
    EXPECT_FALSE(f.files.store("s-1", "../../evil", grayImage(), false).ok());
    EXPECT_EQ(f.files.state("s-1", "../../etc/passwd"), "expired");
    EXPECT_FALSE(f.files.load("..", ok).ok());
    fs::remove_all(dir);
}

TEST(InspectionImageFiles, CleanupRemovesOnlyExpiredCachedImages) {
    const fs::path dir = freshDir("数据");
    ImageFiles f(dir);
    // 三天里的图像：前天两幅、昨天两幅（一早一晚）、今天一幅；另有一幅前天的长期保留、一幅别的工位的
    const std::string old1 = quality::makeRecordId(kNoon - 48 * kHour, 0), old2 = quality::makeRecordId(kNoon - 47 * kHour, 0);
    const std::string early = quality::makeRecordId(kNoon - 30 * kHour, 0), late = quality::makeRecordId(kNoon - 14 * kHour, 0);
    const std::string today = quality::makeRecordId(kNoon, 0), keptOld = quality::makeRecordId(kNoon - 48 * kHour, 5);
    for (const auto& id : {old1, old2, early, late, today}) ASSERT_TRUE(f.files.store("s-1", id, grayImage(), false).ok());
    ASSERT_TRUE(f.files.store("s-1", keptOld, grayImage(), true).ok());
    ASSERT_TRUE(f.files.store("s-2", old1, grayImage(), false).ok());

    // 保留 24 小时：界线落在昨天中午。前天的整天清掉；昨天的只清早上那一幅
    const auto done = f.files.cleanup(kNoon - 24 * kHour, 0);
    EXPECT_EQ(done.expired, 4);  // old1、old2、early，加上二号工位的 old1
    EXPECT_EQ(done.forSpace, 0);
    EXPECT_EQ(f.files.state("s-1", old1), "expired");
    EXPECT_EQ(f.files.state("s-1", early), "expired");
    EXPECT_EQ(f.files.state("s-1", late), "cached");
    EXPECT_EQ(f.files.state("s-1", today), "cached");
    EXPECT_EQ(f.files.state("s-1", keptOld), "kept");  // 长期保留的不受清理影响
    EXPECT_EQ(f.files.state("s-2", old1), "expired");
    EXPECT_FALSE(fs::exists(dir / "inspection-images" / "s-1" / "cache" / "2026-11-22"));  // 空了的那一天，目录也删掉
    EXPECT_EQ(f.files.cleanup(kNoon - 24 * kHour, 0).expired, 0);

    // 什么都没有的时候清理不出错
    ImageFiles empty(freshDir("空的"));
    EXPECT_EQ(empty.files.cleanup(kNoon, 0).expired, 0);
    fs::remove_all(dir);
    fs::remove_all(empty.root);
}

// 磁盘快满了：还没到期的也清，从最旧的一天开始；长期保留的不动
TEST(InspectionImageFiles, WhenTheDiskIsNearlyFullCachedImagesGoEarly) {
    const fs::path dir = freshDir("数据");
    ImageFiles f(dir);
    const std::string yesterday = quality::makeRecordId(kNoon - 24 * kHour, 0), today = quality::makeRecordId(kNoon, 0);
    const std::string kept = quality::makeRecordId(kNoon, 7);
    ASSERT_TRUE(f.files.store("s-1", yesterday, grayImage(), false).ok());
    ASSERT_TRUE(f.files.store("s-1", today, grayImage(), false).ok());
    ASSERT_TRUE(f.files.store("s-1", kept, grayImage(), true).ok());
    // 要求的剩余空间是 1 字节：肯定够，什么都不清
    auto done = f.files.cleanup(kNoon - 72 * kHour, 1);
    EXPECT_EQ(done.expired + done.forSpace, 0);
    EXPECT_FALSE(done.spaceLow);
    // 要求的剩余空间大得不可能满足：缓存清空，并且报告“还是不够”
    done = f.files.cleanup(kNoon - 72 * kHour, ~std::uint64_t{0});
    EXPECT_EQ(done.expired, 0);
    EXPECT_EQ(done.forSpace, 2);
    EXPECT_TRUE(done.spaceLow);
    EXPECT_EQ(f.files.state("s-1", yesterday), "expired");
    EXPECT_EQ(f.files.state("s-1", today), "expired");
    EXPECT_EQ(f.files.state("s-1", kept), "kept");
    fs::remove_all(dir);
}

// 以前的版本把检测图像存在内容仓库里（编号是内容哈希）：升级之后旧记录的图像照样能看
TEST(InspectionImageFiles, ImagesStoredByOlderVersionsStillLoad) {
    const fs::path dir = freshDir("数据");
    ImageFiles f(dir);
    const std::vector<unsigned char> png = {1, 2, 3, 4};
    const std::string sha = f.legacy.put(png).value();
    EXPECT_EQ(f.files.state("s-1", sha), "kept");
    EXPECT_EQ(f.files.load("s-1", sha).value(), png);
    EXPECT_TRUE(f.files.keep("s-1", sha).ok());
    EXPECT_EQ(f.files.cleanup(kNoon + 1000 * kHour, 0).expired, 0);  // 清理不碰内容仓库
    EXPECT_EQ(f.files.state("s-1", sha), "kept");
    EXPECT_EQ(f.files.state("s-1", std::string(64, 'a')), "expired");  // 内容仓库里没有的哈希
    fs::remove_all(dir);
}

// ---------------- 整条路 ----------------

TEST(ImageCacheEndToEnd, TheJanitorCleansInTheBackgroundWithoutTouchingRecordsOrReviewedImages) {
    const fs::path data = freshDir("数据");
    platform::LinePlatform p(data);
    // 三天前的两次检测（都判 OK），一次现在的。直接交给质量分析：这里不关心工位
    auto in = inspection("OK");
    in.frame = grayImage(90);
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string reviewed = quality::makeRecordId(now - 80 * kHour, 0), stale = quality::makeRecordId(now - 80 * kHour, 1);
    for (const auto& id : {reviewed, stale}) {
        auto f = fields(now - 80 * kHour, 0, "OK");
        f.id = id;
        f.imageId = p.inspectionImages.store("s-1", id, in.frame, false).value();
        ASSERT_TRUE(p.inspectionRecords.append(quality::InspectionRecord::create(f).value()).ok());
    }
    const std::string fresh = p.quality.record(in).value();
    ASSERT_TRUE(p.quality.review("s-1", reviewed, {true, "", "下道工序发现的漏检"}).ok());  // 复判过的转成长期保留

    p.quality.setMinFreeBytes(0);  // 这台机器磁盘剩多少不该影响这个测试
    p.imageJanitor.start(std::chrono::seconds(3600));
    bool cleaned = false;
    for (int i = 0; i < 200 && !cleaned; ++i) {
        cleaned = p.quality.getRecord("s-1", stale).value().imageState == "expired";
        if (!cleaned) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(cleaned) << "启动之后应该马上清一次";
    EXPECT_EQ(p.quality.getRecord("s-1", reviewed).value().imageState, "kept");
    EXPECT_TRUE(p.quality.recordImage("s-1", reviewed).ok());  // 三天前的漏检，图像还在
    EXPECT_EQ(p.quality.getRecord("s-1", fresh).value().imageState, "cached");
    EXPECT_EQ(p.quality.query("s-1").size(), 3u);  // 记录一条不少
    p.imageJanitor.stop();
    p.imageJanitor.stop();  // 再停一次不出错
    fs::remove_all(data);
}
