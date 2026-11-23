// BC-9 数据的集成测试（POC-13 第 1 步）：真实的图像文件、内容仓库、JSON 存储，以及和基准图集（BC-4）的配合。
// 重点：按图像内容识别样本；规则 N1 在两个方向上都严格执行。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>

#include "vision/common/path.h"
#include "vision/content/content_store.h"
#include "vision/data/api.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/release/api.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

struct Platform {
    eventbus::InMemoryEventBus bus;
    std::vector<std::string> events;
    persistence::JsonSampleRepository samples;
    persistence::JsonDatasetRepository datasets;
    persistence::JsonGoldenSetRepository goldenRepository;
    content::FileContentStore store;
    content::SampleFiles files;
    data::DataService data;
    content::DataSampleRegistry registry;
    release::GoldenSetService golden;

    explicit Platform(const fs::path& dir)
        : samples(dir), datasets(dir), goldenRepository(dir), store(dir), files(store), data(samples, datasets, files, bus),
          registry(data), golden(goldenRepository, bus) {
        golden.useSampleRegistry(registry);
        bus.subscribe("*", [this](const EventPtr& e) { events.emplace_back(e->name()); });
    }
};

class DataIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir = fs::temp_directory_path() / (std::string("vision-data-") + info->name());
        fs::remove_all(dir);
        fs::create_directories(dir / "images");
        p = std::make_unique<Platform>(dir);
    }
    void TearDown() override {
        p.reset();
        fs::remove_all(dir);
    }

    // 写一幅内容由 seed 决定的图像，返回文件路径
    std::string image(const std::string& name, int seed) {
        cv::Mat m(40, 60, CV_8UC1);
        for (int y = 0; y < m.rows; ++y) {
            for (int x = 0; x < m.cols; ++x) m.at<uchar>(y, x) = static_cast<uchar>((x * 3 + y * 5 + seed * 17) % 251);
        }
        const std::string path = pathToUtf8(dir / "images" / pathFromUtf8(name));
        EXPECT_TRUE(imaging::saveImage(path, imaging::fromMat(m).value()).ok());
        return path;
    }

    std::string copyAs(const std::string& from, const std::string& name) {
        // 程序里的字符串都是 UTF-8。在 Windows 上直接用 std::string 拼路径会按本地编码解释，中文名字就成了乱码
        const fs::path to = dir / "images" / pathFromUtf8(name);
        fs::copy_file(pathFromUtf8(from), to, fs::copy_options::overwrite_existing);
        return pathToUtf8(to);
    }

    release::GoldenSetDefinition goldenOf(const std::vector<std::pair<std::string, std::string>>& idAndPath) {
        release::GoldenSetDefinition def;
        def.name = "基准";
        for (const auto& [id, path] : idAndPath) def.cases.push_back({id, path, false, {}});
        return def;
    }

    fs::path dir;
    std::unique_ptr<Platform> p;
};

}  // namespace

// 通过标准：同一张图像换文件名后再导入，仍然被识别为同一个样本
TEST_F(DataIntegration, SameImageUnderAnotherNameIsTheSameSample) {
    const auto original = image("a.png", 1);
    const auto a = p->data.importSample(original, "ok");
    ASSERT_TRUE(a.ok()) << a.error().message;
    EXPECT_EQ(a.value().size(), 64u);  // 样本 ID 就是内容的 SHA-256

    const auto renamed = p->data.importSample(copyAs(original, "换了个名字.png"));
    ASSERT_TRUE(renamed.ok());
    EXPECT_EQ(renamed.value(), a.value());
    EXPECT_EQ(p->data.listSamples().size(), 1u);

    const auto view = p->data.getSample(a.value()).value();
    EXPECT_EQ(view.names, (std::vector<std::string>{"a.png", "换了个名字.png"}));
    EXPECT_EQ(view.label, "ok");  // 第二次没有给标签：原来的标签保留
    EXPECT_TRUE(fs::exists(pathFromUtf8(view.contentPath)));

    // 内容不同的图像是另一个样本
    const auto other = p->data.importSample(image("b.png", 2), "ng");
    EXPECT_NE(other.value(), a.value());
    EXPECT_EQ(std::count(p->events.begin(), p->events.end(), "SampleImported"), 2);
}

TEST_F(DataIntegration, ConflictingLabelOnReimportIsRejected) {
    const auto path = image("a.png", 1);
    ASSERT_TRUE(p->data.importSample(path, "ok").ok());
    const auto conflict = p->data.importSample(copyAs(path, "b.png"), "ng");
    ASSERT_FALSE(conflict.ok());
    EXPECT_EQ(conflict.error().code, "data.label_conflict");
    EXPECT_FALSE(p->data.importSample(path, "maybe").ok());

    // 不是图像的文件不会被当成样本
    const fs::path text = dir / "images" / "notes.png";
    { std::ofstream(text) << "这不是图像"; }
    EXPECT_EQ(p->data.importSample(pathToUtf8(text)).error().code, "content.not_image");
    EXPECT_FALSE(p->data.importSample(pathToUtf8(dir / "images" / "missing.png")).ok());
    EXPECT_EQ(p->data.listSamples().size(), 1u);
}

TEST_F(DataIntegration, FreezeDatasetMarksItsSamplesAsTraining) {
    const auto a = p->data.importSample(image("a.png", 1), "ok").value();
    const auto b = p->data.importSample(image("b.png", 2), "ng").value();
    const auto dataset = p->data.createDataset("训练用").value();
    ASSERT_TRUE(p->data.addToDataset(dataset, {a, b}).ok());
    const auto version = p->data.freezeDataset(dataset);
    ASSERT_TRUE(version.ok()) << version.error().message;
    EXPECT_EQ(version.value(), dataset + "@v1");

    const auto view = p->data.getDatasetVersion(version.value()).value();
    EXPECT_EQ(view.entries.size(), 2u);
    EXPECT_EQ(view.okCount, 1);
    EXPECT_EQ(view.ngCount, 1);
    EXPECT_TRUE(fs::exists(pathFromUtf8(view.entries[0].contentPath)));  // 训练的时候按这个路径读图像
    EXPECT_EQ(p->data.getSample(a).value().use, "training");
    EXPECT_EQ(std::count(p->events.begin(), p->events.end(), "DatasetVersionFrozen"), 1);

    // 用途确定之后不能改标签
    EXPECT_FALSE(p->data.labelSample(a, "ng").ok());
    // 没有标签的样本不能进冻结的数据集
    const auto c = p->data.importSample(image("c.png", 3)).value();
    ASSERT_TRUE(p->data.addToDataset(dataset, {c}).ok());
    EXPECT_EQ(p->data.freezeDataset(dataset).error().code, "data.unlabeled");
    EXPECT_FALSE(p->data.addToDataset(dataset, {"no-such-sample"}).ok());
    EXPECT_FALSE(p->data.freezeDataset("no-such-dataset").ok());
}

// 基准图集的用例变成了对样本的引用；冻结时它们的用途固定为“留作基准”
TEST_F(DataIntegration, GoldenSetCasesBecomeBenchmarkSamples) {
    const auto id = p->golden.createGoldenSet(goldenOf({{"ok1", image("a.png", 1)}, {"ok2", image("b.png", 2)}}));
    ASSERT_TRUE(id.ok()) << id.error().message;
    const auto set = p->golden.getGoldenSet(id.value()).value();
    ASSERT_EQ(set.cases.size(), 2u);
    EXPECT_EQ(set.cases[0].sampleId.size(), 64u);
    EXPECT_EQ(p->data.getSample(set.cases[0].sampleId).value().use, "benchmark");
    EXPECT_EQ(p->data.getSample(set.cases[0].sampleId).value().label, "ok");

    // 同一批图像再建一个基准图集是允许的：它们本来就是留作基准的
    EXPECT_TRUE(p->golden.createGoldenSet(goldenOf({{"x", image("a.png", 1)}})).ok());
    // 同一幅图像在一个图集里出现两次（哪怕文件名不同）：拒绝
    const auto twice = p->golden.createGoldenSet(goldenOf({{"x", image("a.png", 1)}, {"y", copyAs(image("a.png", 1), "a-copy.png")}}));
    ASSERT_FALSE(twice.ok());
    EXPECT_EQ(twice.error().code, "golden.duplicate_sample");
}

// 通过标准：把同一张图像同时放进基准图集和数据集，后冻结的那个被拒绝。
// 方向一：先冻结了数据集版本（图像用于训练），再想把它放进基准图集
TEST_F(DataIntegration, N1_TrainedImageCannotEnterAGoldenSet) {
    const auto trained = image("trained.png", 1);
    const auto sample = p->data.importSample(trained, "ok").value();
    const auto dataset = p->data.createDataset("训练用").value();
    ASSERT_TRUE(p->data.addToDataset(dataset, {sample}).ok());
    ASSERT_TRUE(p->data.freezeDataset(dataset).ok());

    // 换个文件名也没有用：按内容识别
    const auto fresh = image("fresh.png", 2);
    const auto rejected = p->golden.createGoldenSet(goldenOf({{"a", fresh}, {"b", copyAs(trained, "看起来是新的.png")}}));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "data.n1");
    EXPECT_NE(rejected.error().message.find("trained.png"), std::string::npos);  // 说得出是哪一幅

    EXPECT_TRUE(p->golden.listGoldenSets().empty());  // 被拒绝的图集没有留下来
    // 整体拒绝：一起提交的另一幅没有被标成“留作基准”，以后还可以用于训练
    const auto freshSample = p->data.importSample(fresh).value();
    EXPECT_EQ(p->data.getSample(freshSample).value().use, "unassigned");
    EXPECT_EQ(p->data.getSample(sample).value().use, "training");
}

// 方向二：先冻结了基准图集，再想把其中的图像放进数据集
TEST_F(DataIntegration, N1_BenchmarkImageCannotEnterADatasetVersion) {
    const auto benchmark = image("benchmark.png", 1);
    ASSERT_TRUE(p->golden.createGoldenSet(goldenOf({{"a", benchmark}})).ok());

    const auto same = p->data.importSample(copyAs(benchmark, "换了个名字.png")).value();  // 同一个样本
    const auto other = p->data.importSample(image("other.png", 2), "ok").value();
    const auto dataset = p->data.createDataset("训练用").value();
    ASSERT_TRUE(p->data.addToDataset(dataset, {same, other}).ok());  // 加进草稿是可以的
    const auto rejected = p->data.freezeDataset(dataset);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "data.n1");

    EXPECT_FALSE(p->data.getDatasetVersion(dataset + "@v1").ok());  // 没有生成版本
    EXPECT_EQ(p->data.getSample(other).value().use, "unassigned");  // 另一个样本没有被动过
    EXPECT_EQ(p->data.benchmarkSamplesAmong({same, other}), std::vector<std::string>{same});
}

// 存盘再读回来：样本的用途、数据集版本都还在，N1 照样成立
TEST_F(DataIntegration, SurvivesRestart) {
    const auto benchmark = image("benchmark.png", 1);
    ASSERT_TRUE(p->golden.createGoldenSet(goldenOf({{"a", benchmark}})).ok());
    const auto trained = p->data.importSample(image("trained.png", 2), "ok").value();
    const auto dataset = p->data.createDataset("训练用").value();
    ASSERT_TRUE(p->data.addToDataset(dataset, {trained}).ok());
    const auto version = p->data.freezeDataset(dataset).value();

    p = std::make_unique<Platform>(dir);  // “重启”
    EXPECT_EQ(p->data.getSample(trained).value().use, "training");
    EXPECT_EQ(p->data.getDatasetVersion(version).value().entries.size(), 1u);
    EXPECT_EQ(p->golden.createGoldenSet(goldenOf({{"x", dir.u8string() + "/images/trained.png"}})).error().code, "data.n1");
    const auto again = p->data.importSample(benchmark).value();
    EXPECT_EQ(p->data.getSample(again).value().use, "benchmark");
}

// 没有接上数据上下文的程序：基准图集和以前一样工作（用例没有样本 ID，不检查 N1）
TEST_F(DataIntegration, GoldenSetsWorkWithoutASampleRegistry) {
    release::GoldenSetService bare(p->goldenRepository, p->bus);
    const auto id = bare.createGoldenSet(goldenOf({{"a", image("a.png", 1)}}));
    ASSERT_TRUE(id.ok());
    EXPECT_TRUE(bare.getGoldenSet(id.value()).value().cases[0].sampleId.empty());
    EXPECT_TRUE(p->data.listSamples().empty());
}
