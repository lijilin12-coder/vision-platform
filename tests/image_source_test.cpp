// 图像源（STA-01，docs/07-backend-mvp.md 的 C）：工位绑定图像源，自己取图检测。
//
//   1. 领域层：不变量 S7（图像源只能在工位停止时更换）
//   2. 目录图像源这个适配器
//   3. 整条路：绑定 → 启动 → 一帧帧取图检测，用的是真的文件和整个平台

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>

#include "platform.h"
#include "vision/common/path.h"
#include "vision/image_sources/folder_source.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/station/domain/station.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

// 深色零件上有 spots 个亮斑的图像，存成文件
void writeImage(const fs::path& file, int spots) {
    cv::Mat m(120, 160, CV_8UC1, cv::Scalar(80));
    for (int i = 0; i < spots; ++i) cv::circle(m, {30 + 40 * i, 60}, 8, cv::Scalar(240), cv::FILLED);
    ASSERT_TRUE(imaging::saveImage(pathToUtf8(file), imaging::fromMat(m).value()).ok());
}

fs::path freshDir(const std::string& name) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const fs::path dir = fs::temp_directory_path() / pathFromUtf8("vision-图像源-" + std::string(info->name()) + "-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

}  // namespace

// ---------------- 1. 领域层 ----------------

TEST(StationImageSource, S7_OnlyWhileStopped) {
    using namespace station;
    auto s = Station::create("s-1", "一号").value();
    EXPECT_FALSE(s.imageSource().has_value());
    EXPECT_FALSE(s.bindImageSource({"", "/somewhere"}).ok());
    EXPECT_FALSE(s.bindImageSource({"folder", ""}).ok());
    ASSERT_TRUE(s.bindImageSource({"folder", "/images"}).ok());
    EXPECT_EQ(s.imageSource()->location, "/images");
    EXPECT_TRUE(s.takeDirty());  // 需要保存

    // 让工位运行起来
    ASSERT_TRUE(s.deploy("r-1").ok());
    ASSERT_TRUE(s.markPendingReady("r-1").ok());
    ASSERT_TRUE(s.start().ok());
    // 运行中、暂停中都不能换、不能解除
    auto st = s.bindImageSource({"folder", "/other"});
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, "station.source_while_running");
    EXPECT_FALSE(s.unbindImageSource().ok());
    ASSERT_TRUE(s.pause().ok());
    EXPECT_FALSE(s.bindImageSource({"folder", "/other"}).ok());
    EXPECT_EQ(s.imageSource()->location, "/images");  // 被拒绝的操作不留痕迹

    // 停止之后可以
    ASSERT_TRUE(s.stop().ok());
    ASSERT_TRUE(s.bindImageSource({"folder", "/other"}).ok());
    EXPECT_EQ(s.imageSource()->location, "/other");
    ASSERT_TRUE(s.unbindImageSource().ok());
    EXPECT_FALSE(s.imageSource().has_value());
    EXPECT_TRUE(s.unbindImageSource().ok());  // 本来就没有：不算出错

    // 保存、恢复之后绑定还在
    ASSERT_TRUE(s.bindImageSource({"folder", "/images"}).ok());
    const auto restored = Station::restore(s.snapshot());
    ASSERT_TRUE(restored.imageSource().has_value());
    EXPECT_EQ(restored.imageSource()->kind, "folder");
}

// ---------------- 2. 目录图像源 ----------------

TEST(FolderImageSource, GivesEachImageOnceInNameOrderAndPicksUpNewFiles) {
    const fs::path dir = freshDir("图像");
    writeImage(dir / "b.png", 1);
    writeImage(dir / "a.png", 0);
    writeImage(dir / pathFromUtf8("中文名.png"), 2);
    std::ofstream(dir / "notes.txt") << "不是图像";
    fs::create_directories(dir / "sub");
    writeImage(dir / "sub" / "inner.png", 0);  // 子目录里的不算

    image_sources::ImageSources factory;
    auto opened = factory.open("folder", pathToUtf8(dir));
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    auto& source = *opened.value();

    std::vector<std::string> names;
    for (;;) {
        auto frame = source.next();
        ASSERT_TRUE(frame.ok());
        if (!frame.value()) break;
        names.push_back(frame.value()->name);
        EXPECT_EQ(frame.value()->image.width(), 160);
    }
    EXPECT_EQ(names, (std::vector<std::string>{"a.png", "b.png", "中文名.png"}));
    EXPECT_FALSE(source.next().value().has_value());  // 再问还是没有，不会重复交出去

    // 运行过程中放进来的新图像会被接着处理，哪怕它的名字排在前面
    writeImage(dir / "0-new.png", 0);
    const auto added = source.next();
    ASSERT_TRUE(added.ok());
    ASSERT_TRUE(added.value().has_value());
    EXPECT_EQ(added.value()->name, "0-new.png");
    EXPECT_FALSE(source.next().value().has_value());

    // 重新打开：从头再来
    auto again = factory.open("folder", pathToUtf8(dir));
    EXPECT_EQ(again.value()->next().value()->name, "0-new.png");
    fs::remove_all(dir);
}

TEST(FolderImageSource, BadFilesAreReportedOnceAndDoNotBlockTheRest) {
    const fs::path dir = freshDir("图像");
    writeImage(dir / "a.png", 0);
    std::ofstream(dir / "b.png") << "这不是图像";
    writeImage(dir / "c.JPG", 1);  // 扩展名大小写都认
    image_sources::FolderImageSource source(dir);
    EXPECT_EQ(source.next().value()->name, "a.png");
    const auto bad = source.next();
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, "image_source.bad_frame");
    EXPECT_NE(bad.error().message.find("b.png"), std::string::npos);
    EXPECT_EQ(source.next().value()->name, "c.JPG");  // 坏的那个不会再来一次
    EXPECT_FALSE(source.next().value().has_value());
    fs::remove_all(dir);
}

TEST(FolderImageSource, OpeningChecksKindAndLocation) {
    image_sources::ImageSources factory;
    auto r = factory.open("camera", "0");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, "image_source.unknown_kind");
    r = factory.open("folder", pathToUtf8(fs::temp_directory_path() / "vision-no-such-directory"));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, "image_source.not_found");
    // 目录在打开之后被删掉：取图时报告失败，而不是当成“没有新图像”
    const fs::path dir = freshDir("会被删掉");
    auto opened = factory.open("folder", pathToUtf8(dir));
    ASSERT_TRUE(opened.ok());
    fs::remove_all(dir);
    const auto gone = opened.value()->next();
    ASSERT_FALSE(gone.ok());
    EXPECT_EQ(gone.error().code, "image_source.unreadable");
}

// ---------------- 3. 整条路 ----------------

namespace {

class StationWithSource : public ::testing::Test {
protected:
    void SetUp() override {
        data = freshDir("数据");
        images = freshDir("图像");
        p = std::make_unique<platform::Platform>(data);
        p->stations.allowUnacceptedReleases(true);
        // 输入 → 灰度 → 阈值 → Blob → 个数判定，发布、做成上线包
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
        release = p->releases.createRelease(a.publish(recipe).value()).value();
        station = p->stations.createStation("一号").value();
        ASSERT_TRUE(p->stations.deploy(station, release).ok());
        ASSERT_TRUE(p->stations.waitForLoading(station).ok());
    }
    void TearDown() override {
        p.reset();
        fs::remove_all(data);
        fs::remove_all(images);
    }

    fs::path data, images;
    std::unique_ptr<platform::Platform> p;
    std::string release, station;
};

}  // namespace

TEST_F(StationWithSource, StationPullsFramesFromItsFolderAndInspectsThem) {
    writeImage(images / "1-ok.png", 0);
    writeImage(images / "2-ng.png", 2);
    auto& stations = p->stations;

    // 没有绑定图像源：说清楚缺什么
    ASSERT_TRUE(stations.start(station).ok());
    auto none = stations.inspectNext(station);
    ASSERT_FALSE(none.ok());
    EXPECT_EQ(none.error().code, "station.no_image_source");
    // 运行中不能绑定（S7）
    EXPECT_EQ(stations.bindImageSource(station, "folder", pathToUtf8(images)).error().code, "station.source_while_running");
    ASSERT_TRUE(stations.stop(station).ok());

    // 打不开的图像源在绑定时就被拒绝，不会留下一个坏的绑定
    EXPECT_FALSE(stations.bindImageSource(station, "folder", pathToUtf8(images / "nope")).ok());
    EXPECT_FALSE(stations.bindImageSource(station, "camera", "0").ok());
    EXPECT_TRUE(stations.getStation(station).value().imageSourceKind.empty());

    ASSERT_TRUE(stations.bindImageSource(station, "folder", pathToUtf8(images)).ok());
    EXPECT_EQ(stations.getStation(station).value().imageSourceKind, "folder");
    // 工位没有启动时不能取图
    EXPECT_EQ(stations.inspectNext(station).error().code, "station.not_running");

    ASSERT_TRUE(stations.start(station).ok());
    auto first = stations.inspectNext(station);
    ASSERT_TRUE(first.ok()) << first.error().message;
    ASSERT_TRUE(first.value().has_value());
    EXPECT_EQ(first.value()->frameName, "1-ok.png");
    EXPECT_EQ(first.value()->report.verdict, "OK");
    EXPECT_EQ(first.value()->releaseId, release);
    auto second = stations.inspectNext(station);
    ASSERT_TRUE(second.value().has_value());
    EXPECT_EQ(second.value()->frameName, "2-ng.png");
    EXPECT_EQ(second.value()->report.verdict, "NG");
    EXPECT_EQ(second.value()->report.result.defects.size(), 2u);  // 结果项也一路带出来了
    EXPECT_EQ(second.value()->sequence, first.value()->sequence + 1);
    // 取完了：不是出错，是“现在没有”
    auto empty = stations.inspectNext(station);
    ASSERT_TRUE(empty.ok());
    EXPECT_FALSE(empty.value().has_value());
    // 新放进来的图像接着处理
    writeImage(images / "3-ng.png", 1);
    EXPECT_EQ(stations.inspectNext(station).value()->frameName, "3-ng.png");

    // 停止再启动：图像源重新打开，从头再来
    ASSERT_TRUE(stations.stop(station).ok());
    ASSERT_TRUE(stations.start(station).ok());
    EXPECT_EQ(stations.inspectNext(station).value()->frameName, "1-ok.png");
    ASSERT_TRUE(stations.stop(station).ok());
}

TEST_F(StationWithSource, BindingSurvivesARestartOfTheProgram) {
    writeImage(images / "a.png", 1);
    ASSERT_TRUE(p->stations.bindImageSource(station, "folder", pathToUtf8(images)).ok());
    p.reset();  // 程序退出
    p = std::make_unique<platform::Platform>(data);  // 再启动
    p->stations.allowUnacceptedReleases(true);
    EXPECT_EQ(p->stations.getStation(station).value().imageSourceLocation, pathToUtf8(images));
    // 上次部署的上线包还没来得及生效，重启后在后台重新加载；等它好了再启动
    ASSERT_TRUE(p->stations.waitForLoading(station).ok());
    ASSERT_TRUE(p->stations.start(station).ok());  // 上线包、图像源都还在
    const auto r = p->stations.inspectNext(station);
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value()->report.verdict, "NG");
    // 换一个图像源：下一次取图用的是新的
    ASSERT_TRUE(p->stations.stop(station).ok());
    const fs::path other = freshDir("另一个");
    writeImage(other / "z.png", 0);
    ASSERT_TRUE(p->stations.bindImageSource(station, "folder", pathToUtf8(other)).ok());
    ASSERT_TRUE(p->stations.start(station).ok());
    EXPECT_EQ(p->stations.inspectNext(station).value()->frameName, "z.png");
    ASSERT_TRUE(p->stations.stop(station).ok());
    ASSERT_TRUE(p->stations.unbindImageSource(station).ok());
    fs::remove_all(other);
}

// 一帧读不出来：报告失败，不算一次检测，后面的图像照常处理
TEST_F(StationWithSource, AnUnreadableFrameIsReportedAndSkipped) {
    writeImage(images / "a.png", 0);
    std::ofstream(images / "b.png") << "坏的";
    writeImage(images / "c.png", 0);
    ASSERT_TRUE(p->stations.bindImageSource(station, "folder", pathToUtf8(images)).ok());
    ASSERT_TRUE(p->stations.start(station).ok());
    const auto a = p->stations.inspectNext(station);
    EXPECT_EQ(a.value()->frameName, "a.png");
    const auto bad = p->stations.inspectNext(station);
    ASSERT_FALSE(bad.ok());
    EXPECT_EQ(bad.error().code, "image_source.bad_frame");
    const auto c = p->stations.inspectNext(station);
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value()->frameName, "c.png");
    EXPECT_EQ(c.value()->sequence, a.value()->sequence + 1);  // 坏的那一帧没有占用检测序号
    ASSERT_TRUE(p->stations.stop(station).ok());
}
