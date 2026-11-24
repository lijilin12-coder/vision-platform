// 检测记录（BC-10 质量分析，docs/ddd/14-inspection-records.md）
//
//   1. 领域层：不变量 Q2、Q3，编号，汇总
//   2. 存储：每个工位每天一个文件，一行一条
//   3. 应用服务：编号递增、留图策略
//   4. 整条路：工位检测一次，磁盘上就多一条记录；程序重启之后还查得到

#include <gtest/gtest.h>

#include <filesystem>
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
    std::ifstream in(today);
    int lines = 0;
    for (std::string line; std::getline(in, line);) ++lines;
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

class FakeArchive final : public quality::ImageArchive {
public:
    int stored = 0;
    bool broken = false;
    Result<std::string> store(const Image&) override {
        if (broken) return Result<std::string>::failure("content.write", "磁盘满了");
        return "sha-" + std::to_string(++stored);
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
        for (const auto& r : quality.query("s-1")) kept = (r.imageSha256.empty() ? "-" : r.verdict) + (" " + kept);
        fs::remove_all(dir);
        return kept;
    };
    EXPECT_EQ(imagesKept(quality::ImagePolicy::NgAndError), "- NG ERROR - ");
    EXPECT_EQ(imagesKept(quality::ImagePolicy::None), "- - - - ");
    EXPECT_EQ(imagesKept(quality::ImagePolicy::All), "OK NG ERROR OK ");
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
    EXPECT_TRUE(records[0].imageSha256.empty());
    EXPECT_TRUE(records[1].imageSha256.empty());
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
    ASSERT_FALSE(ng.imageSha256.empty());
    const auto bytes = p.contentStore.get(ng.imageSha256);
    ASSERT_TRUE(bytes.ok());
    const cv::Mat saved = cv::imdecode(bytes.value(), cv::IMREAD_UNCHANGED);
    ASSERT_EQ(saved.cols, 160);
    EXPECT_EQ(saved.at<unsigned char>(60, 30), 240);
    // 合格的不留图像
    EXPECT_TRUE(records[0].imageSha256.empty());
    EXPECT_TRUE(records[2].imageSha256.empty());
    EXPECT_TRUE(records[0].frameName.empty());

    const auto s = p.quality.summary(station);
    EXPECT_EQ(s.total, 3);
    EXPECT_NEAR(*s.yield, 2.0 / 3, 1e-9);
    EXPECT_EQ(s.defectsByCategory.size(), 1u);
    fs::remove_all(data);
}
