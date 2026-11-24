// 模型在检测引擎里运行（POC-13 第 4 步）。
//
// 分三部分：
//   1. 异常检测算子本身：用假的模型，检查它喂给模型的东西、怎么读模型的输出、各种不对的情况
//   2. “模型参数”这条路：方案 → 检测引擎 → 算子，用假的模型来源
//   3. 真的 ONNX Runtime + 一个真的训练出来的模型（tests/data/surface-anomaly.onnx）：
//      得分必须和训练服务（Python）算出来的一致——这是防止“训练和推理的预处理不一样”的那道闸
//      （编译时关掉了推理功能的话，这一部分不存在）

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>
#include <thread>

#include "vision/authoring/api.h"
#include "vision/catalog/api.h"
#include "vision/common/path.h"
#include "vision/content/content_store.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/execution/api.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/models/api.h"
#include "vision/persistence/json_repositories.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/release/api.h"
#ifdef VISION_WITH_ONNXRUNTIME
#include "vision/inference_onnx/onnx_model_source.h"
#endif

using namespace vision;
namespace fs = std::filesystem;

namespace {

// 假的模型：记下收到的输入；输出“得分 = 输入里最大的数，异常图 = 输入本身”
class FakeModel final : public InferenceModel {
public:
    explicit FakeModel(ModelInfo info) : info_(std::move(info)) {}
    const ModelInfo& info() const override { return info_; }
    Result<std::vector<Tensor>> run(const Tensor& input) const override {
        received = input;
        if (fail) return Result<std::vector<Tensor>>::failure("inference.failed", "模拟：模型运行失败");
        if (!outputs.empty()) return outputs;
        Tensor score{{1}, {*std::max_element(input.data.begin(), input.data.end())}};
        Tensor map{input.shape, input.data};
        return std::vector<Tensor>{score, map};
    }
    ModelInfo info_;
    mutable Tensor received;
    bool fail = false;
    std::vector<Tensor> outputs;  // 不为空时原样交出（用来模拟不守约定的模型）
};

ModelInfo anomalyInfo(int width = 8, int height = 4, double threshold = 0.5) {
    ModelInfo info;
    info.id = "fake@v1";
    info.kind = "anomaly-autoencoder";
    info.inputWidth = width;
    info.inputHeight = height;
    info.numbers = {{"threshold", threshold}};
    return info;
}

struct OpResult {
    Status status = Status::success();
    std::map<std::string, Value> outputs;
};

OpResult runAnomaly(const Image& image, const InferenceModel* model, std::optional<Roi> roi = std::nullopt) {
    const auto& catalog = catalog::OperatorCatalog::global();
    const auto meta = catalog.find("vision.anomaly_detect");
    EXPECT_TRUE(meta.has_value());
    ParamMap params = meta->signature.defaultParams();
    std::map<std::string, std::shared_ptr<const InferenceModel>> models;
    if (model != nullptr) {
        params["model"] = model->info().id;
        models["model"] = std::shared_ptr<const InferenceModel>(model, [](const InferenceModel*) {});  // 不接管所有权
    }
    auto op = catalog.create("vision.anomaly_detect", 1);
    EXPECT_TRUE(op.ok());
    const std::map<std::string, Value> inputs = {{"image", image}};
    catalog::OpContext ctx(inputs, params, std::move(roi), nullptr, &models);
    OpResult r;
    r.status = op.value()->execute(ctx);
    r.outputs = ctx.outputs();
    return r;
}

Image gray(const cv::Mat& m) { return imaging::fromMat(m).value(); }

// 和 tests/data/surface-anomaly.onnx 的训练数据同一种样子的图像，但不含随机的东西：
// 这样 Python 里能画出一模一样的图像，算出用来对比的得分（画法见本文件末尾的说明）。
cv::Mat surfaceImage(bool defect) {
    cv::Mat m(480, 640, CV_8UC1, cv::Scalar(150));
    for (int x = 120; x < 520; ++x) m(cv::Rect(x, 90, 1, 300)).setTo(70 + (12 * (x - 120)) / 400);
    if (defect) m(cv::Rect(300, 200, 40, 24)).setTo(30);
    return m;
}

}  // namespace

// ---------------- 1. 算子 ----------------

TEST(AnomalyDetectOp, FeedsTheModelAResizedUnitRangeImageAndReportsRatio) {
    // 16×8 的图像，左半 0、右半 255；模型输入 8×4 → 每个模型像素正好是 2×2 个原图像素的平均
    cv::Mat m(8, 16, CV_8UC1, cv::Scalar(0));
    m(cv::Rect(8, 0, 8, 8)).setTo(255);
    FakeModel model(anomalyInfo(8, 4, 0.5));
    const auto r = runAnomaly(gray(m), &model);
    ASSERT_TRUE(r.status.ok()) << r.status.error().message;

    EXPECT_EQ(model.received.shape, (std::vector<std::int64_t>{1, 1, 4, 8}));  // 批、通道、高、宽
    ASSERT_EQ(model.received.data.size(), 32u);
    EXPECT_FLOAT_EQ(model.received.data[0], 0.0f);
    EXPECT_FLOAT_EQ(model.received.data[7], 1.0f);  // 255 → 1.0

    EXPECT_DOUBLE_EQ(std::get<double>(r.outputs.at("score")), 1.0);
    EXPECT_DOUBLE_EQ(std::get<double>(r.outputs.at("ratio")), 2.0);  // 得分 1.0 ÷ 阈值 0.5
    // 异常图：回到原图的尺寸；128 对应阈值，所以 1.0（两倍阈值）是 255（截断），0 是 0
    const cv::Mat map = imaging::view(std::get<Image>(r.outputs.at("map")));
    EXPECT_EQ(map.size(), m.size());
    EXPECT_EQ(map.at<uchar>(4, 0), 0);
    EXPECT_EQ(map.at<uchar>(4, 15), 255);
}

TEST(AnomalyDetectOp, DownscalingAveragesPixelsLikeTheTrainerDoes) {
    // 4×4 的棋盘格缩到 2×2：INTER_AREA 是取平均，每个像素都是 (0 + 255) / 2；
    // 换成别的插值方法（比如最近邻、双线性）结果就不是这个数——训练时用的是 INTER_AREA，这里必须一样
    cv::Mat m(4, 4, CV_8UC1);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) m.at<uchar>(y, x) = (x + y) % 2 == 0 ? 0 : 255;
    }
    FakeModel model(anomalyInfo(2, 2, 0.5));
    ASSERT_TRUE(runAnomaly(gray(m), &model).status.ok());
    for (const float v : model.received.data) EXPECT_NEAR(v, 0.5f, 0.003f);
}

TEST(AnomalyDetectOp, RejectsWhatItCannotHandle) {
    const Image image = gray(cv::Mat(8, 16, CV_8UC1, cv::Scalar(100)));
    const auto message = [](const OpResult& r) { return r.status.ok() ? std::string("（成功了）") : r.status.error().message; };

    EXPECT_NE(message(runAnomaly(image, nullptr)).find("还没有设置"), std::string::npos);

    cv::Mat color(8, 16, CV_8UC3, cv::Scalar(1, 2, 3));
    FakeModel ok(anomalyInfo());
    EXPECT_NE(message(runAnomaly(imaging::fromMat(color).value(), &ok)).find("灰度"), std::string::npos);
    EXPECT_NE(message(runAnomaly(image, &ok, Roi::rect(0, 0, 4, 4).value())).find("ROI"), std::string::npos);

    auto info = anomalyInfo();
    info.kind = "classifier";
    FakeModel wrongKind(info);
    EXPECT_NE(message(runAnomaly(image, &wrongKind)).find("只会用"), std::string::npos);

    info = anomalyInfo();
    info.numbers.clear();
    FakeModel noThreshold(info);
    EXPECT_NE(message(runAnomaly(image, &noThreshold)).find("阈值"), std::string::npos);

    info = anomalyInfo();
    info.inputWidth = 0;
    FakeModel noSize(info);
    EXPECT_NE(message(runAnomaly(image, &noSize)).find("契约"), std::string::npos);

    FakeModel failing(anomalyInfo());
    failing.fail = true;
    EXPECT_NE(message(runAnomaly(image, &failing)).find("运行失败"), std::string::npos);

    // 不守约定的输出：只有一个输出；异常图的大小不对；得分不是有效的数
    FakeModel oneOutput(anomalyInfo());
    oneOutput.outputs = {Tensor{{1}, {0.1f}}};
    EXPECT_NE(message(runAnomaly(image, &oneOutput)).find("约定"), std::string::npos);
    FakeModel wrongMap(anomalyInfo());
    wrongMap.outputs = {Tensor{{1}, {0.1f}}, Tensor{{1, 1, 2, 2}, {0, 0, 0, 0}}};
    EXPECT_NE(message(runAnomaly(image, &wrongMap)).find("约定"), std::string::npos);
    FakeModel nan(anomalyInfo());
    nan.outputs = {Tensor{{1}, {std::nanf("")}}, Tensor{{1, 1, 4, 8}, std::vector<float>(32, 0.f)}};
    EXPECT_NE(message(runAnomaly(image, &nan)).find("有效"), std::string::npos);
}

// ---------------- 2. 模型参数：方案 → 检测引擎 → 算子 ----------------

namespace {

class FakeModelSource final : public execution::ModelSource {
public:
    Result<std::shared_ptr<const InferenceModel>> load(const std::string& id, bool requirePublished) override {
        requests.emplace_back(id, requirePublished);
        if (id != "fake@v1") return Result<std::shared_ptr<const InferenceModel>>::failure("models.not_found", "找不到模型版本 " + id);
        if (requirePublished && !published) {
            return Result<std::shared_ptr<const InferenceModel>>::failure("inference.model_unpublished", "还没有发布");
        }
        return std::shared_ptr<const InferenceModel>(std::make_shared<FakeModel>(anomalyInfo(8, 4, 0.5)));
    }
    bool published = false;
    std::vector<std::pair<std::string, bool>> requests;
};

struct Engine {
    eventbus::InMemoryEventBus bus;
    persistence::JsonRecipeRepository recipes;
    persistence::JsonRecipeVersionRepository versions;
    authoring::AuthoringService authoring;
    execution::ExecutionService execution;

    explicit Engine(const fs::path& dir)
        : recipes(dir), versions(dir), authoring(recipes, versions, catalog::OperatorCatalog::global(), bus),
          execution(catalog::OperatorCatalog::global(), bus) {}

    // 输入 → 灰度 → 异常检测 → 范围判定（ratio 不超过 1）。返回 {方案 ID, 异常检测节点 ID}
    std::pair<std::string, std::string> anomalyRecipe() {
        const auto recipe = authoring.createRecipe("异常检测").value();
        const auto input = authoring.addNode(recipe, "vision.image_input").value();
        const auto toGray = authoring.addNode(recipe, "vision.to_gray").value();
        const auto anomaly = authoring.addNode(recipe, "vision.anomaly_detect").value();
        const auto judge = authoring.addNode(recipe, "vision.judge_range").value();
        EXPECT_TRUE(authoring.connect(recipe, input + ".image", toGray + ".image").ok());
        EXPECT_TRUE(authoring.connect(recipe, toGray + ".image", anomaly + ".image").ok());
        EXPECT_TRUE(authoring.connect(recipe, anomaly + ".ratio", judge + ".value").ok());
        EXPECT_TRUE(authoring.setParam(recipe, judge, "min", 0.0).ok());
        EXPECT_TRUE(authoring.setParam(recipe, judge, "max", 1.0).ok());
        return {recipe, anomaly};
    }
};

class ModelParam : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir = fs::temp_directory_path() / (std::string("vision-inference-") + info->name());
        fs::remove_all(dir);
        e = std::make_unique<Engine>(dir);
    }
    void TearDown() override {
        e.reset();
        fs::remove_all(dir);
    }
    fs::path dir;
    std::unique_ptr<Engine> e;
};

}  // namespace

// 规则 P3：需要模型的节点没有指明模型版本，方案不能发布
TEST_F(ModelParam, RecipeCannotBePublishedUntilTheModelIsChosen) {
    const auto [recipe, anomaly] = e->anomalyRecipe();
    const auto rejected = e->authoring.publish(recipe);
    ASSERT_FALSE(rejected.ok());
    EXPECT_NE(rejected.error().message.find("模型参数 model 还没有设置"), std::string::npos);

    EXPECT_FALSE(e->authoring.setParam(recipe, anomaly, "model", 3.0).ok());  // 模型参数的值是文本
    ASSERT_TRUE(e->authoring.setParam(recipe, anomaly, "model", std::string("fake@v1")).ok());
    const auto version = e->authoring.publish(recipe);
    ASSERT_TRUE(version.ok()) << version.error().message;
    // 模型版本 ID 随方案版本冻结，保存再读回来也还在
    const auto doc = e->authoring.getVersionDocument(version.value()).value();
    bool found = false;
    for (const auto& n : doc.nodes) {
        if (n.operatorId == "vision.anomaly_detect") found = std::get<std::string>(n.params.at("model")) == "fake@v1";
    }
    EXPECT_TRUE(found);
}

TEST_F(ModelParam, DebugRunMayUseUnpublishedModelsButInspectionMayNot) {
    FakeModelSource source;
    e->execution.useModelSource(source);
    const auto [recipe, anomaly] = e->anomalyRecipe();
    ASSERT_TRUE(e->authoring.setParam(recipe, anomaly, "model", std::string("fake@v1")).ok());
    const Image dark = gray(cv::Mat(8, 16, CV_8UC1, cv::Scalar(51)));     // 51/255 = 0.2 → ratio 0.4 → 合格
    const Image bright = gray(cv::Mat(8, 16, CV_8UC1, cv::Scalar(204)));  // 0.8 → ratio 1.6 → 不合格

    // 调试运行：未发布的模型可以用
    const auto draft = e->authoring.getDraftDocument(recipe).value();
    const auto debug = e->execution.debugRun(draft, dark);
    ASSERT_TRUE(debug.ok()) << debug.error().message;
    EXPECT_EQ(debug.value().report.verdict, "OK");
    ASSERT_FALSE(source.requests.empty());
    EXPECT_FALSE(source.requests.back().second);
    EXPECT_NEAR(std::get<double>(debug.value().outputs.at(anomaly).at("ratio")), 0.4, 1e-6);

    // 正式检测：必须是已发布的模型。加载方案时就报错，而不是等到第一幅图像
    const auto version = e->authoring.publish(recipe).value();
    const auto doc = e->authoring.getVersionDocument(version).value();
    const auto prepared = e->execution.prepare(doc);
    ASSERT_FALSE(prepared.ok());
    EXPECT_EQ(prepared.error().code, "inference.model_unpublished");
    EXPECT_TRUE(source.requests.back().second);
    EXPECT_FALSE(e->execution.inspect(doc, dark).ok());

    source.published = true;
    ASSERT_TRUE(e->execution.prepare(doc).ok());
    EXPECT_EQ(e->execution.inspect(doc, dark).value().verdict, "OK");
    EXPECT_EQ(e->execution.inspect(doc, bright).value().verdict, "NG");
    // 模型在加载方案时取一次，之后每次检测不再去取
    const auto loads = source.requests.size();
    EXPECT_EQ(e->execution.inspect(doc, dark).value().verdict, "OK");
    EXPECT_EQ(source.requests.size(), loads);
}

TEST_F(ModelParam, ProblemsAreReportedWhenTheRecipeIsLoaded) {
    const auto [recipe, anomaly] = e->anomalyRecipe();
    const Image image = gray(cv::Mat(8, 16, CV_8UC1, cv::Scalar(51)));

    // 模型还没选：调试运行照样能跑，上游节点有输出，只有这个节点失败
    auto debug = e->execution.debugRun(e->authoring.getDraftDocument(recipe).value(), image);
    ASSERT_TRUE(debug.ok()) << debug.error().message;
    EXPECT_EQ(debug.value().report.verdict, "ERROR");
    EXPECT_TRUE(debug.value().outputs.count(anomaly) == 0 || debug.value().outputs.at(anomaly).empty());
    EXPECT_GE(debug.value().outputs.size(), 2u);

    // 选了模型，但这个程序没有推理引擎
    ASSERT_TRUE(e->authoring.setParam(recipe, anomaly, "model", std::string("nope@v9")).ok());
    debug = e->execution.debugRun(e->authoring.getDraftDocument(recipe).value(), image);
    ASSERT_FALSE(debug.ok());
    EXPECT_EQ(debug.error().code, "execution.no_model_source");

    // 有推理引擎，但模型仓库里没有这个模型版本
    FakeModelSource source;
    e->execution.useModelSource(source);
    debug = e->execution.debugRun(e->authoring.getDraftDocument(recipe).value(), image);
    ASSERT_FALSE(debug.ok());
    EXPECT_EQ(debug.error().code, "models.not_found");
    EXPECT_NE(debug.error().message.find("nope@v9"), std::string::npos);
}

// 用了还没发布的模型的方案版本，不能做成上线包（到了工位上也只会加载失败，不如在这里就拦住）
TEST_F(ModelParam, ReleaseIsRefusedWhileTheRecipeCannotBeLoaded) {
    struct Source final : release::RecipeVersionSource {
        explicit Source(const authoring::AuthoringService& a) : authoring(a) {}
        Result<authoring::RecipeVersionDocument> find(const std::string& id) const override { return authoring.getVersionDocument(id); }
        const authoring::AuthoringService& authoring;
    } recipeSource(e->authoring);
    struct Check final : release::LoadCheck {
        explicit Check(execution::ExecutionService& x) : execution(x) {}
        Status canLoad(const authoring::RecipeVersionDocument& recipe) override { return execution.prepare(recipe); }
        execution::ExecutionService& execution;
    } check(e->execution);
    persistence::JsonReleaseRepository releaseRepository(dir);
    release::ReleaseService releases(releaseRepository, recipeSource, e->bus);
    releases.useLoadCheck(check);

    FakeModelSource source;
    e->execution.useModelSource(source);
    const auto [recipe, anomaly] = e->anomalyRecipe();
    ASSERT_TRUE(e->authoring.setParam(recipe, anomaly, "model", std::string("fake@v1")).ok());
    const auto version = e->authoring.publish(recipe).value();

    const auto refused = releases.createRelease(version);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "inference.model_unpublished");
    EXPECT_TRUE(releases.listReleases().empty());

    source.published = true;  // 模型发布之后，同一个方案版本就可以了
    EXPECT_TRUE(releases.createRelease(version).ok());
}

// ---------------- 3. 真的 ONNX Runtime ----------------

#ifdef VISION_WITH_ONNXRUNTIME

namespace {

struct RealEngine {
    Engine engine;
    persistence::JsonModelVersionRepository modelVersions;
    content::FileContentStore store;
    content::ModelFileStore modelFiles;
    models::ModelService models;
    inference_onnx::OnnxModelSource source;

    explicit RealEngine(const fs::path& dir)
        : engine(dir), modelVersions(dir), store(dir), modelFiles(store), models(modelVersions, modelFiles, engine.bus), source(models) {
        engine.execution.useModelSource(source);
    }

    // 把 tests/data 里的模型导入模型仓库。阈值是训练它的那次训练报告里的数
    std::string importSurfaceModel(const std::string& file = std::string(VISION_SOURCE_DIR) + "/tests/data/surface-anomaly.onnx") {
        models::ContractView contract;
        contract.kind = "anomaly-autoencoder";
        contract.inputWidth = 256;
        contract.inputHeight = 256;
        contract.inputChannels = 1;
        contract.output = "anomaly-score-and-map";
        contract.numbers = {{"threshold", 0.080625}};
        models::ProvenanceView provenance;
        provenance.kind = "imported";
        const auto id = models.importModel("surface-anomaly", file, contract, provenance);
        EXPECT_TRUE(id.ok()) << (id.ok() ? "" : id.error().message);
        return id.ok() ? id.value() : "";
    }
};

class OnnxInference : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        // 中文目录：模型文件的路径里有中文时也要能加载
        dir = fs::temp_directory_path() / pathFromUtf8(std::string("vision-推理-") + info->name());
        fs::remove_all(dir);
        r = std::make_unique<RealEngine>(dir);
    }
    void TearDown() override {
        r.reset();
        fs::remove_all(dir);
    }
    fs::path dir;
    std::unique_ptr<RealEngine> r;
};

}  // namespace

// 【最重要的一个测试】同一个模型、同一幅图像，这里算出的得分必须和训练服务（Python）算出的一样。
// 下面两个数是在 Python 里用 services/training/trainer.py 同样的预处理算出来的：
//
//   m = np.full((480, 640), 150, np.uint8)
//   for x in range(120, 520): m[90:390, x] = 70 + (12 * (x - 120)) // 400
//   # 有缺陷的那幅再加：m[200:224, 300:340] = 30
//   g = cv2.resize(m, (256, 256), interpolation=cv2.INTER_AREA).astype(np.float32) / 255
//   score = onnxruntime.InferenceSession("tests/data/surface-anomaly.onnx").run(None, {"image": g[None, None]})[0][0]
//
// 这个测试失败，说明推理一侧的预处理和训练一侧不一样了：模型在产线上的表现会和训练报告里的不一样，而且不会有任何报错。
TEST_F(OnnxInference, ScoresMatchTheTrainingService) {
    const auto modelId = r->importSurfaceModel();
    const auto [recipe, anomaly] = r->engine.anomalyRecipe();
    ASSERT_TRUE(r->engine.authoring.setParam(recipe, anomaly, "model", modelId).ok());
    const auto draft = r->engine.authoring.getDraftDocument(recipe).value();

    const auto good = r->engine.execution.debugRun(draft, gray(surfaceImage(false)));
    ASSERT_TRUE(good.ok()) << good.error().message;
    EXPECT_NEAR(std::get<double>(good.value().outputs.at(anomaly).at("score")), 0.055857397615909576, 2e-5);
    EXPECT_EQ(good.value().report.verdict, "OK");

    const auto bad = r->engine.execution.debugRun(draft, gray(surfaceImage(true)));
    ASSERT_TRUE(bad.ok()) << bad.error().message;
    EXPECT_NEAR(std::get<double>(bad.value().outputs.at(anomaly).at("score")), 0.09900777041912079, 2e-5);
    EXPECT_EQ(bad.value().report.verdict, "NG");

    // 异常图上最亮的地方就在缺陷那里（缺陷在 x 300～340、y 200～224）
    const cv::Mat map = imaging::view(std::get<Image>(bad.value().outputs.at(anomaly).at("map")));
    cv::Point brightest;
    cv::minMaxLoc(map, nullptr, nullptr, nullptr, &brightest);
    EXPECT_TRUE(cv::Rect(290, 190, 60, 44).contains(brightest)) << brightest.x << "," << brightest.y;
}

TEST_F(OnnxInference, UnpublishedModelsOnlyRunInDebugAndPublishingTakesEffectWithoutRestart) {
    const auto modelId = r->importSurfaceModel();
    EXPECT_EQ(modelId, "surface-anomaly@v1");
    ASSERT_TRUE(r->source.load(modelId, /*requirePublished=*/false).ok());
    const auto refused = r->source.load(modelId, /*requirePublished=*/true);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "inference.model_unpublished");
    // 模型已经加载过了；发布之后不用重新启动就能正式使用，而且用的是同一个已经加载好的模型
    ASSERT_TRUE(r->models.publish(modelId).ok());
    const auto a = r->source.load(modelId, true), b = r->source.load(modelId, false);
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.value().get(), b.value().get());
    EXPECT_EQ(a.value()->info().kind, "anomaly-autoencoder");
    EXPECT_EQ(a.value()->info().inputWidth, 256);
}

TEST_F(OnnxInference, RefusesMissingCorruptAndNonOnnxModels) {
    EXPECT_FALSE(r->source.load("nobody@v1", false).ok());

    // 不是 ONNX 的文件：能导入模型仓库（仓库不懂文件的内容），加载时被推理引擎拒绝
    const fs::path junk = dir / "junk.onnx";
    fs::create_directories(dir);
    std::ofstream(junk, std::ios::binary) << "这不是一个模型";
    const auto junkId = r->importSurfaceModel(pathToUtf8(junk));
    const auto notOnnx = r->source.load(junkId, false);
    ASSERT_FALSE(notOnnx.ok());
    EXPECT_EQ(notOnnx.error().code, "inference.load_failed");

    // 模型文件在仓库里被改了一个字节：哈希对不上，拒绝加载
    const auto goodId = r->importSurfaceModel();
    const auto view = r->models.getModelVersion(goodId).value();
    {
        std::fstream file(pathFromUtf8(view.filePath), std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(1000);
        file.put('\x7f');
    }
    const auto corrupt = r->source.load(goodId, false);
    ASSERT_FALSE(corrupt.ok());
    EXPECT_EQ(corrupt.error().code, "inference.model_corrupt");
}

// 模型可以被几个线程同时运行（产线上几条检测流程可能共用一个模型）
TEST_F(OnnxInference, OneModelCanRunOnSeveralThreadsAtOnce) {
    const auto model = r->source.load(r->importSurfaceModel(), false);
    ASSERT_TRUE(model.ok());
    Tensor input{{1, 1, 256, 256}, std::vector<float>(256 * 256, 0.3f)};
    const float expected = model.value()->run(input).value()[0].data[0];
    std::vector<std::thread> threads;
    std::atomic<int> wrong{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 10; ++i) {
                const auto out = model.value()->run(input);
                if (!out.ok() || out.value()[0].data[0] != expected) ++wrong;
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(wrong.load(), 0);

    // 形状和数据对不上的输入被拒绝，不会让推理引擎读到数组外面去
    EXPECT_FALSE(model.value()->run(Tensor{{1, 1, 256, 256}, std::vector<float>(10, 0.f)}).ok());
    EXPECT_FALSE(model.value()->run(Tensor{{1, 1, 64, 64}, std::vector<float>(64 * 64, 0.f)}).ok());  // 尺寸不是模型要的
}

#endif  // VISION_WITH_ONNXRUNTIME
