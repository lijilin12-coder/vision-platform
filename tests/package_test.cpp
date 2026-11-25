// 方案包（REL-02，H-06，docs/ddd/12-recipe-package.md）。
//
//   1. 导入规则 K1～K4（领域层，纯规则）
//   2. 包文件这个适配器：写了再读回来一样；被改过、被截断、混进怪东西的文件都被拒绝
//   3. 整条路：两个互不相干的数据目录当作两台机器，从一台导出、在另一台导入、上线、检测

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>

#include "platform.h"
#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/release/domain/recipe_package.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

fs::path freshDir(const std::string& name) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const fs::path dir = fs::temp_directory_path() / pathFromUtf8("vision-方案包-" + std::string(info->name()) + "-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::vector<unsigned char> readAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& file, const std::vector<unsigned char>& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// ---------------- 1. 导入规则 ----------------

namespace {

release::PackageFacts facts() {
    release::PackageFacts f;
    f.schema = release::kRecipePackageSchema;
    f.recipeVersionId = "r@v1";
    f.recipeSha256 = "aaa";
    f.operators = {{"vision.to_gray", 1}, {"vision.threshold", 1}};
    f.models = {{"m@v1", "h1"}, {"n@v2", "h2"}};
    return f;
}

release::LocalInventory emptyMachine() {
    release::LocalInventory local;
    local.operators = {{"vision.to_gray", 1}, {"vision.threshold", 1}, {"vision.blob", 1}};
    return local;
}

}  // namespace

TEST(PackageImportRules, FreshMachineGetsEverything) {
    const auto plan = release::planImport(facts(), emptyMachine());
    ASSERT_TRUE(plan.ok()) << plan.error().message;
    EXPECT_TRUE(plan.value().addRecipeVersion);
    EXPECT_EQ(plan.value().modelsToAdd, (std::vector<std::string>{"m@v1", "n@v2"}));
    EXPECT_TRUE(plan.value().modelsAlreadyHere.empty());
}

TEST(PackageImportRules, K1_UnknownSchemaIsRefused) {
    auto f = facts();
    f.schema = "vision.recipe-package/9.0";
    EXPECT_EQ(release::planImport(f, emptyMachine()).error().code, "release.package_schema");
    f = facts();
    f.recipeVersionId.clear();
    EXPECT_EQ(release::planImport(f, emptyMachine()).error().code, "release.package_invalid");
}

TEST(PackageImportRules, K2_MissingOrIncompatibleOperatorsAreRefusedAndAllAreListed) {
    auto local = emptyMachine();
    local.operators.erase("vision.to_gray");
    local.operators["vision.threshold"] = 2;  // 主版本不同
    const auto plan = release::planImport(facts(), local);
    ASSERT_FALSE(plan.ok());
    EXPECT_EQ(plan.error().code, "release.package_missing_operator");
    // 两个问题都说出来，不是只报第一个
    EXPECT_NE(plan.error().message.find("vision.to_gray"), std::string::npos);
    EXPECT_NE(plan.error().message.find("vision.threshold"), std::string::npos);
    EXPECT_NE(plan.error().message.find("主版本"), std::string::npos);
}

TEST(PackageImportRules, K3_SameIdDifferentContentIsAConflict) {
    auto local = emptyMachine();
    local.recipeSha256 = "bbb";  // 同一个方案版本 ID，内容不同
    EXPECT_EQ(release::planImport(facts(), local).error().code, "release.package_conflict");

    local = emptyMachine();
    local.modelSha256["n@v2"] = "different";
    const auto plan = release::planImport(facts(), local);
    ASSERT_FALSE(plan.ok());
    EXPECT_EQ(plan.error().code, "release.package_conflict");
    EXPECT_NE(plan.error().message.find("n@v2"), std::string::npos);
}

TEST(PackageImportRules, K4_ReimportIsANoOpAndPartialImportsComplete) {
    auto local = emptyMachine();
    local.recipeSha256 = "aaa";
    local.modelSha256 = {{"m@v1", "h1"}, {"n@v2", "h2"}};
    auto plan = release::planImport(facts(), local);
    ASSERT_TRUE(plan.ok());
    EXPECT_TRUE(plan.value().nothingToDo());
    EXPECT_EQ(plan.value().modelsAlreadyHere.size(), 2u);

    // 上一次导入中途断了：模型有一个、方案版本还没有 → 这一次补齐剩下的
    local.recipeSha256.reset();
    local.modelSha256.erase("n@v2");
    plan = release::planImport(facts(), local);
    ASSERT_TRUE(plan.ok());
    EXPECT_TRUE(plan.value().addRecipeVersion);
    EXPECT_EQ(plan.value().modelsToAdd, (std::vector<std::string>{"n@v2"}));
}

// ---------------- 2、3. 文件和整条路 ----------------

namespace {

class Package : public ::testing::Test {
protected:
    void SetUp() override {
        dirA = freshDir("构建机");
        dirB = freshDir("产线机");
        work = freshDir("文件");
        a = std::make_unique<platform::Platform>(dirA);
        b = std::make_unique<platform::Platform>(dirB);
        b->stations.allowUnacceptedReleases(true);
        file = pathToUtf8(work / pathFromUtf8("方案.vpkg"));
    }
    void TearDown() override {
        a.reset();
        b.reset();
        for (const auto& d : {dirA, dirB, work}) fs::remove_all(d);
    }

    // 有固定图案的样图：每个位置都不一样，框出来的模板不会认错
    static Image sample() {
        cv::Mat m(120, 160, CV_8UC1);
        for (int y = 0; y < m.rows; ++y) {
            for (int x = 0; x < m.cols; ++x) m.at<uchar>(y, x) = static_cast<uchar>((x * 7 + y * 13) % 251);
        }
        return imaging::fromMat(m).value();
    }

    // 在构建机上：输入 → 模板匹配 → 范围判定（相似度），带一个匹配模板。返回方案版本 ID
    std::string publishLocatingRecipe() {
        auto& au = a->authoring;
        const auto recipe = au.createRecipe("定位").value();
        const auto input = au.addNode(recipe, "vision.image_input").value();
        const auto match = au.addNode(recipe, "vision.template_match").value();
        const auto judge = au.addNode(recipe, "vision.judge_range").value();
        EXPECT_TRUE(au.connect(recipe, input + ".image", match + ".image").ok());
        EXPECT_TRUE(au.connect(recipe, match + ".score", judge + ".value").ok());
        EXPECT_TRUE(au.setParam(recipe, judge, "min", 0.7).ok());
        EXPECT_TRUE(au.setParam(recipe, judge, "max", 1.0).ok());
        EXPECT_TRUE(au.setParam(recipe, match, "angleRange", 0.0).ok());
        const auto t = au.createMatchTemplate(recipe, "定位孔", sample(), 40, 30, 48, 32);
        EXPECT_TRUE(t.ok()) << (t.ok() ? "" : t.error().message);
        EXPECT_TRUE(au.setParam(recipe, match, "template", t.value()).ok());
        const auto version = au.publish(recipe);
        EXPECT_TRUE(version.ok()) << (version.ok() ? "" : version.error().message);
        return version.value();
    }

    // 在构建机上：输入 → 灰度 → 异常检测（用 tests/data 里的模型）→ 范围判定。返回方案版本 ID
    std::string publishModelRecipe(bool publishModel = true) {
        models::ContractView contract;
        contract.kind = "anomaly-autoencoder";
        contract.inputWidth = contract.inputHeight = 256;
        contract.output = "anomaly-score-and-map";
        contract.numbers = {{"threshold", 0.080625}};
        models::ProvenanceView provenance;
        provenance.kind = "imported";
        const auto model = a->models.importModel("surface", std::string(VISION_SOURCE_DIR) + "/tests/data/surface-anomaly.onnx", contract,
                                                 provenance);
        EXPECT_TRUE(model.ok());
        if (publishModel) {
            EXPECT_TRUE(a->models.publish(model.value()).ok());
        }
        auto& au = a->authoring;
        const auto recipe = au.createRecipe("表面").value();
        const auto input = au.addNode(recipe, "vision.image_input").value();
        const auto gray = au.addNode(recipe, "vision.to_gray").value();
        const auto anomaly = au.addNode(recipe, "vision.anomaly_detect").value();
        const auto judge = au.addNode(recipe, "vision.judge_range").value();
        EXPECT_TRUE(au.connect(recipe, input + ".image", gray + ".image").ok());
        EXPECT_TRUE(au.connect(recipe, gray + ".image", anomaly + ".image").ok());
        EXPECT_TRUE(au.connect(recipe, anomaly + ".ratio", judge + ".value").ok());
        EXPECT_TRUE(au.setParam(recipe, judge, "min", 0.0).ok());
        EXPECT_TRUE(au.setParam(recipe, judge, "max", 1.0).ok());
        EXPECT_TRUE(au.setParam(recipe, anomaly, "model", model.value()).ok());
        return au.publish(recipe).value();
    }

    fs::path dirA, dirB, work;
    std::unique_ptr<platform::Platform> a, b;
    std::string file;
};

}  // namespace

// 带模板的方案：导出 → 另一台机器导入 → 做成上线包 → 工位上检测，结果和原来的机器一样
TEST_F(Package, RecipeWithTemplateTravelsToAnotherMachineAndRunsThere) {
    const auto version = publishLocatingRecipe();
    const auto exported = a->packages.exportPackage(version, file);
    ASSERT_TRUE(exported.ok()) << exported.error().message;
    EXPECT_EQ(exported.value().templates, 1);
    EXPECT_EQ(exported.value().models, 0);

    // 只看不导入
    const auto manifest = b->packages.inspect(file);
    ASSERT_TRUE(manifest.ok());
    EXPECT_EQ(manifest.value().recipeVersionId, version);
    EXPECT_EQ(manifest.value().recipeName, "定位");
    EXPECT_EQ(manifest.value().templates[0].name, "定位孔");
    EXPECT_FALSE(b->releases.createRelease(version).ok());  // 还没导入：产线机不认识这个方案版本

    const auto imported = b->packages.importPackage(file);
    ASSERT_TRUE(imported.ok()) << imported.error().message;
    EXPECT_TRUE(imported.value().recipeAdded);
    EXPECT_EQ(imported.value().templates, 1);
    // 导入的方案版本不进方案编排：那边没有多出可以编辑的方案
    EXPECT_TRUE(b->authoring.listRecipes().empty());

    // 上线、部署、检测
    const auto release = b->releases.createRelease(version);
    ASSERT_TRUE(release.ok()) << release.error().message;
    const auto station = b->stations.createStation("产线一号").value();
    ASSERT_TRUE(b->stations.deploy(station, release.value()).ok());
    ASSERT_TRUE(b->stations.waitForLoading(station).ok());
    ASSERT_TRUE(b->stations.start(station).ok());
    const auto there = b->stations.inspect(station, sample());
    ASSERT_TRUE(there.ok()) << there.error().message;
    EXPECT_EQ(there.value().report.verdict, "OK");
    ASSERT_EQ(there.value().report.result.poses.size(), 1u);
    EXPECT_TRUE(there.value().report.result.poses[0].found);
    ASSERT_TRUE(b->stations.stop(station).ok());

    // 和构建机上的结果一样
    const auto here = a->execution.inspect(a->authoring.getVersionDocument(version).value(), sample());
    EXPECT_EQ(here.value().verdict, there.value().report.verdict);
    EXPECT_DOUBLE_EQ(here.value().result.poses[0].score, there.value().report.result.poses[0].score);
}

#ifdef VISION_WITH_ONNXRUNTIME
// 带模型的方案：模型文件、契约、“已发布”的状态一起带过去
TEST_F(Package, RecipeWithModelBringsTheModelAlong) {
    const auto version = publishModelRecipe();
    const auto exported = a->packages.exportPackage(version, file);
    ASSERT_TRUE(exported.ok()) << exported.error().message;
    EXPECT_EQ(exported.value().models, 1);

    const auto imported = b->packages.importPackage(file);
    ASSERT_TRUE(imported.ok()) << imported.error().message;
    EXPECT_EQ(imported.value().modelsAdded, (std::vector<std::string>{"surface@v1"}));
    const auto model = b->models.getModelVersion("surface@v1").value();
    EXPECT_EQ(model.status, "published");
    EXPECT_EQ(model.sha256, a->models.getModelVersion("surface@v1").value().sha256);
    EXPECT_DOUBLE_EQ(model.contract.numbers.at("threshold"), 0.080625);
    EXPECT_EQ(model.contract.inputWidth, 256);

    // 产线机上直接能做成上线包并检测（模型已发布，推理引擎加载得了）
    ASSERT_TRUE(b->releases.createRelease(version).ok());
    cv::Mat m(480, 640, CV_8UC1, cv::Scalar(150));
    for (int x = 120; x < 520; ++x) m(cv::Rect(x, 90, 1, 300)).setTo(70 + (12 * (x - 120)) / 400);
    const auto doc = b->importedRecipes.find(version).value();
    EXPECT_EQ(b->execution.inspect(doc, imaging::fromMat(m).value()).value().verdict, "OK");
    m(cv::Rect(300, 200, 40, 24)).setTo(30);
    EXPECT_EQ(b->execution.inspect(doc, imaging::fromMat(m).value()).value().verdict, "NG");
}

TEST_F(Package, UnpublishedModelsCannotBeExported) {
    const auto version = publishModelRecipe(/*publishModel=*/false);
    const auto exported = a->packages.exportPackage(version, file);
    ASSERT_FALSE(exported.ok());
    EXPECT_EQ(exported.error().code, "release.package_unpublished_model");
    EXPECT_FALSE(fs::exists(pathFromUtf8(file)));  // 没有留下半个文件
}

// 回归：部署了一个用到模型的上线包之后马上退出程序。后台还在加载（要用推理引擎），
// 而推理引擎声明在工位服务后面、会先被销毁——以前这里会崩溃（pure virtual method called）。
TEST_F(Package, ShuttingDownWhileAReleaseWithAModelIsStillLoadingIsSafe) {
    const auto version = publishModelRecipe();
    ASSERT_TRUE(a->packages.exportPackage(version, file).ok());
    ASSERT_TRUE(b->packages.importPackage(file).ok());
    const auto release = b->releases.createRelease(version).value();
    const auto station = b->stations.createStation("产线一号").value();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(b->stations.deploy(station, release).ok());  // 后台开始加载
        b.reset();                                                // 不等它，直接退出
        b = std::make_unique<platform::Platform>(dirB);
        b->stations.allowUnacceptedReleases(true);
        // 重新启动后，没来得及生效的部署会在后台重新加载；这一次也不等
        ASSERT_TRUE(b->stations.getStation(station).ok());
        b.reset();
        b = std::make_unique<platform::Platform>(dirB);
        b->stations.allowUnacceptedReleases(true);
    }
    ASSERT_TRUE(b->stations.waitForLoading(station).ok());
    EXPECT_TRUE(b->stations.start(station).ok());
    EXPECT_TRUE(b->stations.stop(station).ok());
}
#endif

TEST_F(Package, ReimportIsHarmlessAndConflictsAreRefused) {
    const auto version = publishLocatingRecipe();
    ASSERT_TRUE(a->packages.exportPackage(version, file).ok());
    ASSERT_TRUE(b->packages.importPackage(file).ok());
    // 再导入一次：什么都不变，不出错
    const auto again = b->packages.importPackage(file);
    ASSERT_TRUE(again.ok()) << again.error().message;
    EXPECT_TRUE(again.value().nothingNew());
    EXPECT_EQ(b->importedRecipes.listIds().size(), 1u);
    // 导回它出来的那台机器：那里本来就有这个方案版本（在方案编排里），同样不出错，也不多存一份
    const auto home = a->packages.importPackage(file);
    ASSERT_TRUE(home.ok()) << home.error().message;
    EXPECT_TRUE(home.value().nothingNew());
    EXPECT_TRUE(a->importedRecipes.listIds().empty());
    // 同一份内容导出两次：包 ID 不同，但内容相同，仍然算重复
    const std::string second = pathToUtf8(work / "second.vpkg");
    ASSERT_TRUE(a->packages.exportPackage(version, second).ok());
    EXPECT_NE(a->packages.inspect(second).value().packageId, a->packages.inspect(file).value().packageId);
    EXPECT_TRUE(b->packages.importPackage(second).value().nothingNew());

    // 冲突：产线机上已经有同一个 ID、内容不同的方案版本
    auto tampered = b->importedRecipes.find(version).value();
    tampered.recipeName = "被改过的";
    ASSERT_TRUE(b->importedRecipes.save(tampered).ok());
    const auto conflict = b->packages.importPackage(file);
    ASSERT_FALSE(conflict.ok());
    EXPECT_EQ(conflict.error().code, "release.package_conflict");
    EXPECT_EQ(b->importedRecipes.find(version).value().recipeName, "被改过的");  // 被拒绝的导入没有覆盖任何东西
}

TEST_F(Package, ExportFailuresAreExplained) {
    EXPECT_FALSE(a->packages.exportPackage("nobody@v1", file).ok());
    const auto version = publishLocatingRecipe();
    // 目标目录不存在
    EXPECT_FALSE(a->packages.exportPackage(version, pathToUtf8(work / "no-such-dir" / "x.vpkg")).ok());
    // 模板的图像在内容仓库里被弄丢了：导出时就发现，而不是带着一个残缺的包出门
    for (const auto& e : fs::recursive_directory_iterator(dirA / "content")) {
        if (e.is_regular_file()) {
            fs::remove(e.path());
            break;
        }
    }
    const auto exported = a->packages.exportPackage(version, file);
    ASSERT_FALSE(exported.ok());
    EXPECT_NE(exported.error().message.find("定位孔"), std::string::npos);
}

// 包文件被动过手脚：每一种都必须被拒绝，而且什么都不留下
TEST_F(Package, DamagedOrDoctoredFilesAreRefusedAndLeaveNothingBehind) {
    const auto version = publishLocatingRecipe();
    ASSERT_TRUE(a->packages.exportPackage(version, file).ok());
    const std::vector<unsigned char> good = readAll(pathFromUtf8(file));
    ASSERT_GT(good.size(), 2048u);
    ASSERT_EQ(good.size() % 512, 0u);

    const fs::path bad = work / "bad.vpkg";
    const auto refuse = [&](const std::vector<unsigned char>& bytes, const char* what) {
        writeAll(bad, bytes);
        const auto r = b->packages.importPackage(pathToUtf8(bad));
        EXPECT_FALSE(r.ok()) << what;
        if (!r.ok()) {
            EXPECT_EQ(r.error().code, "exchange.corrupt") << what << "：" << r.error().message;
        }
        EXPECT_TRUE(b->importedRecipes.listIds().empty()) << what;
        EXPECT_FALSE(fs::exists(dirB / "content")) << what << "：内容仓库里不应该多出东西";
    };

    // 找到三个文件各自的位置：头在 512 的整数倍上，文件名在头的最前面
    const auto headerOf = [&](const std::string& prefix) {
        for (std::size_t at = 0; at + 512 <= good.size(); at += 512) {
            if (std::equal(prefix.begin(), prefix.end(), good.begin() + static_cast<std::ptrdiff_t>(at))) return at;
        }
        return std::size_t(0) - 1;
    };
    const std::size_t recipeAt = headerOf("recipe-version.json"), contentAt = headerOf("content/");
    ASSERT_NE(recipeAt, std::size_t(0) - 1);
    ASSERT_NE(contentAt, std::size_t(0) - 1);

    auto bytes = good;
    bytes[contentAt + 512 + 100] ^= 0x01;  // 模板图像里改一个比特
    refuse(bytes, "模板图像被改了一个比特");

    bytes = good;
    bytes[recipeAt + 512 + 20] ^= 0x01;  // 方案版本文档里改一个比特
    refuse(bytes, "方案版本被改了");

    bytes = good;
    bytes[recipeAt + 3] ^= 0x01;  // 文件头被改：头的校验和不对
    refuse(bytes, "文件头被改了");

    bytes.assign(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2 / 512 * 512));
    refuse(bytes, "文件被截断");

    bytes.assign(good.begin(), good.end() - 1024);  // 没有结尾标记
    refuse(bytes, "没有结尾标记");

    refuse({}, "空文件");
    refuse(std::vector<unsigned char>(4096, 'x'), "根本不是方案包");

    // 不存在的文件：是“打不开”，不是“损坏”
    EXPECT_EQ(b->packages.importPackage(pathToUtf8(work / "nope.vpkg")).error().code, "exchange.read");

    // 原样的文件仍然能导入：上面那些失败没有把什么弄坏
    EXPECT_TRUE(b->packages.importPackage(file).ok());
}

// 同样的内容导出两次，除了包 ID 之外字节完全相同（文件里不写时间）
TEST_F(Package, ExportIsDeterministicApartFromThePackageId) {
    const auto version = publishLocatingRecipe();
    const std::string one = pathToUtf8(work / "1.vpkg"), two = pathToUtf8(work / "2.vpkg");
    ASSERT_TRUE(a->packages.exportPackage(version, one).ok());
    ASSERT_TRUE(a->packages.exportPackage(version, two).ok());
    const auto x = readAll(pathFromUtf8(one)), y = readAll(pathFromUtf8(two));
    ASSERT_EQ(x.size(), y.size());
    std::size_t different = 0;
    for (std::size_t i = 0; i < x.size(); ++i) different += x[i] != y[i];
    EXPECT_GT(different, 0u);
    EXPECT_LT(different, 48u);  // 只有包 ID（36 个字符）和清单那个头的校验和不同
}
