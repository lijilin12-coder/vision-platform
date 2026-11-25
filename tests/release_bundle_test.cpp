// 上线包文件（REL-03，docs/ddd/16-release-bundle.md）
//
//   1. 领域层：接收一个上线包的规则 B1、B2
//   2. 整条路：方案构建平台上验收、导出 → 文件 → 产线运行平台上导入、部署、检测。
//      产线那一侧用的是 LinePlatform，而且没有打开调试的口子：走的就是正式的规则

#include <gtest/gtest.h>

#include <filesystem>
#include <opencv2/imgproc.hpp>

#include "platform.h"
#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/release/domain/release.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

// 一条通过了的验收记录：6 个用例，标准是不许漏检、不许误报、不许出错
release::AcceptanceRecord passedRecord() {
    release::AcceptanceRecord a;
    a.goldenSetId = "gs-1";
    a.goldenSetName = "亮斑基准";
    a.checkedAtMs = 1000;
    a.passed = true;
    a.total = 6;
    a.truthOk = 4;
    a.truthNg = 2;
    return a;
}

}  // namespace

// ---------------- 1. 领域层 ----------------

TEST(ReleaseBundleRules, B1_OnlyAnAcceptedReleaseIsAdopted) {
    using namespace release;
    auto adopted = Release::adopt("rel-1", "recipe@v1", {}, passedRecord());
    ASSERT_TRUE(adopted.ok());
    auto& r = adopted.value();
    EXPECT_EQ(r.id(), "rel-1");  // ID 不变：两台机器上说的是同一个上线包
    EXPECT_TRUE(r.deployable());
    EXPECT_EQ(r.acceptance()->goldenSetName, "亮斑基准");
    const auto events = r.pullEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_STREQ(events[0]->name(), "ReleaseImported");

    const auto refused = [](const std::function<void(AcceptanceRecord&)>& change) {
        auto a = passedRecord();
        change(a);
        const auto r = Release::adopt("rel-1", "recipe@v1", {}, a);
        return !r.ok() && r.error().code == "release.bundle_not_accepted";
    };
    EXPECT_TRUE(refused([](auto& a) { a.passed = false; }));
    // 文件可以手工改。光把“通过”改成真不够：数字和标准要对得上
    EXPECT_TRUE(refused([](auto& a) { a.misses = 1; }));                         // 漏了 1 张，标准是 0
    EXPECT_TRUE(refused([](auto& a) { a.falseAlarms = 1; }));                    // 误报率 25%，标准是 0
    EXPECT_TRUE(refused([](auto& a) { a.errors = 1; }));
    EXPECT_TRUE(refused([](auto& a) { a.total = 0; a.truthOk = 0; a.truthNg = 0; }));  // 一个用例都没有
    EXPECT_TRUE(refused([](auto& a) { a.total = 9; }));                          // 合格 + 不合格 ≠ 总数
    EXPECT_TRUE(refused([](auto& a) { a.misses = -1; }));
    // 数字在标准之内：可以
    auto a = passedRecord();
    a.misses = 1;
    a.maxMisses = 1;
    EXPECT_TRUE(Release::adopt("rel-1", "recipe@v1", {}, a).ok());

    EXPECT_EQ(Release::adopt("", "recipe@v1", {}, passedRecord()).error().code, "release.bundle_invalid");
    EXPECT_EQ(Release::adopt("rel-1", "", {}, passedRecord()).error().code, "release.bundle_invalid");
}

TEST(ReleaseBundleRules, B2_SameIdMustMeanTheSameRelease) {
    using namespace release;
    const auto r = Release::adopt("rel-1", "recipe@v1", {}, passedRecord()).value();
    EXPECT_TRUE(r.sameAs("recipe@v1").ok());
    const auto st = r.sameAs("recipe@v2");
    ASSERT_FALSE(st.ok());
    EXPECT_EQ(st.error().code, "release.bundle_conflict");
}

// ---------------- 2. 整条路 ----------------

namespace {

class ReleaseBundle : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root = fs::temp_directory_path() / pathFromUtf8("vision-上线包文件-" + std::string(info->name()));
        fs::remove_all(root);
        fs::create_directories(root / "ok");
        fs::create_directories(root / "ng");
        writeImage(root / "ok" / "1.png", 0, 80);
        writeImage(root / "ok" / "2.png", 0, 90);
        writeImage(root / "ng" / "1.png", 1);
        writeImage(root / "ng" / "2.png", 3);
        builder = std::make_unique<platform::Platform>(root / pathFromUtf8("构建机"));
        line = std::make_unique<platform::LinePlatform>(root / pathFromUtf8("产线机"));
        auto& a = builder->authoring;
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
        strictVersion = a.publish(recipe).value();
        strict = builder->releases.createRelease(strictVersion).value();
        ASSERT_TRUE(a.setParam(recipe, n[4], "maxCount", 2.0).ok());
        lenient = builder->releases.createRelease(a.publish(recipe).value()).value();
        golden = platform::createGoldenSetFromFolders(*builder, "亮斑基准", pathToUtf8(root / "ok"), pathToUtf8(root / "ng"), {}).value();
        file = pathToUtf8(root / pathFromUtf8("亮斑.vrel"));
    }
    void TearDown() override {
        builder.reset();
        line.reset();
        fs::remove_all(root);
    }
    static Image image(int spots, int background = 80) {
        cv::Mat m(120, 160, CV_8UC1, cv::Scalar(background));
        for (int i = 0; i < spots; ++i) cv::circle(m, {30 + 40 * i, 60}, 8, cv::Scalar(240), cv::FILLED);
        return imaging::fromMat(m).value();
    }
    static void writeImage(const fs::path& f, int spots, int background = 80) {
        ASSERT_TRUE(imaging::saveImage(pathToUtf8(f), image(spots, background)).ok());
    }

    fs::path root;
    std::unique_ptr<platform::Platform> builder;
    std::unique_ptr<platform::LinePlatform> line;
    std::string strict, strictVersion, lenient, golden, file;
};

}  // namespace

TEST_F(ReleaseBundle, B1_OnlyADeployableReleaseCanBeExported) {
    // 还没验收
    auto r = builder->packages.exportRelease(strict, file);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, "release.not_accepted");
    EXPECT_NE(r.error().message.find("还没有做过验收检查"), std::string::npos);
    // 验收没通过
    ASSERT_FALSE(builder->releases.checkAcceptance(lenient, golden).value().passed);
    r = builder->packages.exportRelease(lenient, file);
    ASSERT_FALSE(r.ok());
    EXPECT_NE(r.error().message.find("没有通过"), std::string::npos);
    EXPECT_FALSE(fs::exists(pathFromUtf8(file)));  // 被拒绝的不留下文件
    EXPECT_EQ(builder->packages.exportRelease("nobody", file).error().code, "release.not_found");
    // 通过了：可以
    ASSERT_TRUE(builder->releases.checkAcceptance(strict, golden).value().passed);
    r = builder->packages.exportRelease(strict, file);
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(r.value().recipeVersionId, strictVersion);
    // 文件里看得到它带着哪个上线包、验收的结果
    const auto manifest = builder->packages.inspect(file).value();
    ASSERT_TRUE(manifest.release.has_value());
    EXPECT_EQ(manifest.release->releaseId, strict);
    EXPECT_TRUE(manifest.release->acceptance.passed);
    EXPECT_EQ(manifest.release->acceptance.total, 4);
    // 撤回了：不能再导出
    ASSERT_TRUE(builder->releases.withdraw(strict).ok());
    EXPECT_EQ(builder->packages.exportRelease(strict, file).error().code, "release.withdrawn");
}

TEST_F(ReleaseBundle, TravelsToTheLineWithItsAcceptanceAndIsDeployedUnderTheRealRule) {
    ASSERT_TRUE(builder->releases.checkAcceptance(strict, golden).value().passed);
    ASSERT_TRUE(builder->packages.exportRelease(strict, file).ok());
    builder.reset();  // 构建机关掉了：产线那边只有这个文件

    std::vector<std::string> events;
    line->bus.subscribe("*", [&](const EventPtr& e) { events.push_back(e->name()); });
    const auto imported = line->packages.importRelease(file);
    ASSERT_TRUE(imported.ok()) << imported.error().message;
    EXPECT_EQ(imported.value().releaseId, strict);  // 同一个 ID
    EXPECT_TRUE(imported.value().releaseAdded);
    EXPECT_TRUE(imported.value().package.recipeAdded);
    EXPECT_EQ(std::count(events.begin(), events.end(), "ReleaseImported"), 1);

    // 验收记录跟着过来了，不用（也不能）在产线上重做
    const auto a = line->releases.getAcceptance(strict);
    ASSERT_TRUE(a.ok());
    EXPECT_TRUE(a.value().passed);
    EXPECT_EQ(a.value().goldenSetName, "亮斑基准");
    EXPECT_EQ(a.value().total, 4);
    EXPECT_EQ(a.value().checkedAtMs, imported.value().acceptance.checkedAtMs);
    EXPECT_EQ(line->releases.getManifest(strict).value().acceptance, "passed");

    // 正式的规则（没有打开调试的口子）：可以部署，能检测
    const std::string station = line->stations.createStation("一号").value();
    ASSERT_TRUE(line->stations.deploy(station, strict).ok());
    ASSERT_TRUE(line->stations.waitForLoading(station).ok());
    ASSERT_TRUE(line->stations.start(station).ok());
    EXPECT_EQ(line->stations.inspect(station, image(0)).value().report.verdict, "OK");
    const auto ng = line->stations.inspect(station, image(2)).value();
    EXPECT_EQ(ng.report.verdict, "NG");
    EXPECT_EQ(ng.releaseId, strict);
    ASSERT_TRUE(line->stations.stop(station).ok());

    // 在产线上自己创建的上线包没有验收记录：部署不了
    const std::string local = line->releases.createRelease(strictVersion).value();
    EXPECT_EQ(line->stations.deploy(station, local).error().code, "station.release_not_accepted");

    // 重启之后都还在
    line.reset();
    line = std::make_unique<platform::LinePlatform>(root / pathFromUtf8("产线机"));
    EXPECT_EQ(line->releases.getManifest(strict).value().acceptance, "passed");
    EXPECT_EQ(line->stations.getStation(station).value().activeRelease, strict);
}

TEST_F(ReleaseBundle, B2_ImportingAgainChangesNothing) {
    ASSERT_TRUE(builder->releases.checkAcceptance(strict, golden).value().passed);
    ASSERT_TRUE(builder->packages.exportRelease(strict, file).ok());
    ASSERT_TRUE(line->packages.importRelease(file).value().releaseAdded);
    // 产线上后来把它撤回了。再导入一次同一个文件：不能把它“复活”
    ASSERT_TRUE(line->releases.withdraw(strict).ok());
    const auto again = line->packages.importRelease(file);
    ASSERT_TRUE(again.ok()) << again.error().message;
    EXPECT_FALSE(again.value().releaseAdded);
    EXPECT_TRUE(again.value().package.nothingNew());
    EXPECT_TRUE(line->releases.getManifest(strict).value().withdrawn);
    EXPECT_EQ(line->releases.listReleases().size(), 1u);
    // 在构建机上导入自己导出的文件：已经有了，什么都不做
    EXPECT_FALSE(builder->packages.importRelease(file).value().releaseAdded);
}

TEST_F(ReleaseBundle, WhatIsRefusedOnImport) {
    // 普通的方案包里没有上线包：说清楚该用哪个命令
    const std::string package = pathToUtf8(root / pathFromUtf8("方案.vpkg"));
    ASSERT_TRUE(builder->packages.exportPackage(strictVersion, package).ok());
    auto r = line->packages.importRelease(package);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, "release.bundle_no_release");
    EXPECT_TRUE(line->releases.listReleases().empty());
    EXPECT_FALSE(line->recipeSource.find(strictVersion).ok());  // 被拒绝的什么都不留下
    EXPECT_FALSE(line->packages.importRelease(pathToUtf8(root / "nope.vrel")).ok());

    // 同一个 ID、不同的方案版本（B2）；验收记录对不上（B1）——直接交给上线管理，绕过文件
    ASSERT_TRUE(builder->releases.checkAcceptance(strict, golden).value().passed);
    auto bundled = builder->releases.bundle(strict).value();
    auto tampered = bundled;
    tampered.acceptance.misses = 1;  // 标准是不许漏检，却写着“通过”
    EXPECT_EQ(builder->releases.adopt(tampered).error().code, "release.bundle_not_accepted");
    auto other = bundled;
    other.recipeVersionId = builder->releases.getManifest(lenient).value().recipeVersionId;
    EXPECT_EQ(builder->releases.adopt(other).error().code, "release.bundle_conflict");
    auto unknown = bundled;
    unknown.releaseId = "rel-x";
    unknown.recipeVersionId = "nobody@v1";  // 它引用的方案版本本机没有
    EXPECT_FALSE(builder->releases.adopt(unknown).ok());
    EXPECT_FALSE(builder->releases.getManifest("rel-x").ok());
}

// 上线包文件也可以当方案包导入：只拿到方案版本。那样的话上线包要自己创建，没有验收记录
TEST_F(ReleaseBundle, ImportedAsAPlainPackageBringsOnlyTheRecipe) {
    ASSERT_TRUE(builder->releases.checkAcceptance(strict, golden).value().passed);
    ASSERT_TRUE(builder->packages.exportRelease(strict, file).ok());
    const auto r = line->packages.importPackage(file);
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_TRUE(r.value().recipeAdded);
    EXPECT_TRUE(line->releases.listReleases().empty());
    // 之后再按上线包文件导入：方案版本已经有了，上线包是新的
    const auto bundle = line->packages.importRelease(file);
    ASSERT_TRUE(bundle.ok());
    EXPECT_TRUE(bundle.value().releaseAdded);
    EXPECT_FALSE(bundle.value().package.recipeAdded);
}
