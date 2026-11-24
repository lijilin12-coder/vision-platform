// 训练闭环的集成测试（POC-13 第 2 步）：数据 → 训练任务 → 模型仓库，真实的文件存储。
// 训练后端是假的（按脚本报告进度）；真正的训练服务在第 3 步。

#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>

#include "vision/common/path.h"
#include "vision/content/content_store.h"
#include "vision/data/api.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/models/api.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/release/api.h"
#include "vision/training/api.h"
#include "vision/training/domain/training_job.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

// 假的训练后端：start 记下收到的请求；之后每次 poll 交出脚本里的下一个状态
class ScriptedBackend final : public training::TrainingBackend {
public:
    std::string name() const override { return "scripted"; }
    Result<std::string> start(const training::TrainingRequest& request) override {
        if (!available) return Result<std::string>::failure("backend.unreachable", "模拟：训练服务没有启动");
        requests.push_back(request);
        return std::string("ticket-") + std::to_string(requests.size());
    }
    Result<training::BackendProgress> poll(const std::string& ticket) override {
        polled.push_back(ticket);
        if (script.empty()) return Result<training::BackendProgress>::failure("backend.unreachable", "模拟：查询超时");
        const auto next = script.front();
        script.pop_front();
        return next;
    }
    Status cancel(const std::string& ticket) override {
        cancelled.push_back(ticket);
        return Status::success();
    }

    void running(double progress) {
        training::BackendProgress p;
        p.progress = progress;
        script.push_back(p);
    }
    void completed(const std::string& modelFile) {
        training::BackendProgress p;
        p.state = training::BackendProgress::State::Completed;
        p.progress = 1;
        p.report = {{{"test_auroc", 0.96}, {"val_loss", 0.004}}, 42.0, "epochs=5"};
        p.output = {modelFile, "anomaly-autoencoder", 64, 64, 1, "reconstruction", {{"threshold", 0.021}}};
        script.push_back(p);
    }
    void failed(const std::string& why) {
        training::BackendProgress p;
        p.state = training::BackendProgress::State::Failed;
        p.failure = why;
        script.push_back(p);
    }

    bool available = true;
    std::vector<training::TrainingRequest> requests;
    std::vector<std::string> polled, cancelled;
    std::deque<training::BackendProgress> script;
};

struct Platform {
    eventbus::InMemoryEventBus bus;
    std::vector<std::string> events;
    persistence::JsonSampleRepository samples;
    persistence::JsonDatasetRepository datasets;
    persistence::JsonModelVersionRepository modelVersions;
    persistence::JsonTrainingJobRepository jobs;
    persistence::JsonGoldenSetRepository goldenRepository;
    content::FileContentStore store;
    content::SampleFiles sampleFiles;
    content::ModelFileStore modelFiles;
    data::DataService data;
    models::ModelService models;
    content::DataDatasetSource datasetSource;
    content::RepositoryModelImporter importer;
    training::TrainingService training;

    Platform(const fs::path& dir, training::TrainingBackend& backend)
        : samples(dir), datasets(dir), modelVersions(dir), jobs(dir), goldenRepository(dir), store(dir), sampleFiles(store),
          modelFiles(store), data(samples, datasets, sampleFiles, bus), models(modelVersions, modelFiles, bus), datasetSource(data),
          importer(models), training(jobs, backend, datasetSource, importer, bus) {
        bus.subscribe("*", [this](const EventPtr& e) { events.emplace_back(e->name()); });
    }
    int count(const std::string& name) const { return static_cast<int>(std::count(events.begin(), events.end(), name)); }
};

class TrainingLoop : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir = fs::temp_directory_path() / (std::string("vision-training-") + info->name());
        fs::remove_all(dir);
        fs::create_directories(dir / "images");
        p = std::make_unique<Platform>(dir, backend);
    }
    void TearDown() override {
        p.reset();
        fs::remove_all(dir);
    }

    std::string image(const std::string& name, int seed) {
        cv::Mat m(32, 32, CV_8UC1);
        for (int y = 0; y < m.rows; ++y) {
            for (int x = 0; x < m.cols; ++x) m.at<uchar>(y, x) = static_cast<uchar>((x * 3 + y * 5 + seed * 17) % 251);
        }
        const std::string path = pathToUtf8(dir / "images" / name);
        EXPECT_TRUE(imaging::saveImage(path, imaging::fromMat(m).value()).ok());
        return path;
    }

    // 20 个合格 + 4 个不合格的样本，冻结成一个数据集版本
    std::string frozenDataset(int ok = 20, int ng = 4) {
        const auto dataset = p->data.createDataset("表面").value();
        std::vector<std::string> ids;
        for (int i = 0; i < ok; ++i) ids.push_back(p->data.importSample(image("ok" + std::to_string(i) + ".png", i), "ok").value());
        for (int i = 0; i < ng; ++i) ids.push_back(p->data.importSample(image("ng" + std::to_string(i) + ".png", 100 + i), "ng").value());
        EXPECT_TRUE(p->data.addToDataset(dataset, ids).ok());
        return p->data.freezeDataset(dataset).value();
    }

    // 一个假的“模型文件”：第 2 步不关心它的内容，只关心它被原样存进了模型仓库
    std::string modelFile(const std::string& content = "fake-onnx-bytes") {
        const fs::path path = dir / "trained.onnx";
        { std::ofstream(path, std::ios::binary) << content; }
        return pathToUtf8(path);
    }

    training::SubmitRequest request(const std::string& datasetVersion) {
        training::SubmitRequest r;
        r.datasetVersionId = datasetVersion;
        r.modelName = "surface";
        r.settings = {{"epochs", 5}};
        return r;
    }

    fs::path dir;
    ScriptedBackend backend;
    std::unique_ptr<Platform> p;
};

}  // namespace

// 通过标准：提交训练任务 → 完成 → 模型版本自动出现在模型仓库，来源信息完整
TEST_F(TrainingLoop, CompletedJobBecomesAnUnpublishedModelVersionWithFullProvenance) {
    const auto dataset = frozenDataset();
    const auto job = p->training.submit(request(dataset));
    ASSERT_TRUE(job.ok()) << job.error().message;
    EXPECT_EQ(p->training.getJob(job.value()).value().status, "running");

    // 训练后端拿到的是已经划分好的样本，带着图像文件的位置
    ASSERT_EQ(backend.requests.size(), 1u);
    const auto& sent = backend.requests[0];
    EXPECT_EQ(sent.samples.size(), 24u);
    EXPECT_EQ(sent.config.modelKind, "anomaly-autoencoder");
    EXPECT_EQ(sent.config.settings.at("epochs"), 5);
    int train = 0, val = 0, test = 0;
    for (const auto& s : sent.samples) {
        EXPECT_TRUE(fs::exists(pathFromUtf8(s.contentPath)));
        ++(s.split == training::Split::Train ? train : s.split == training::Split::Validation ? val : test);
    }
    EXPECT_EQ(train, 20);  // 合格 20 个：16 / 2 / 2；不合格只有 4 个，按 10% 四舍五入是 0 个，全部进了训练集
    EXPECT_EQ(val + test, 4);

    backend.running(0.4);
    backend.completed(modelFile());
    const auto halfway = p->training.refresh(job.value()).value();
    EXPECT_EQ(halfway.status, "running");
    EXPECT_DOUBLE_EQ(halfway.progress, 0.4);
    EXPECT_TRUE(p->models.listModelVersions().empty());

    const auto done = p->training.refresh(job.value()).value();
    EXPECT_EQ(done.status, "completed");
    EXPECT_EQ(done.metrics.at("test_auroc"), 0.96);
    EXPECT_EQ(done.modelVersionId, "surface@v1");

    // 模型版本：未发布；文件原样进了内容仓库；契约和来源完整（T-5）
    const auto model = p->models.getModelVersion("surface@v1").value();
    EXPECT_EQ(model.status, "unpublished");
    EXPECT_TRUE(fs::exists(pathFromUtf8(model.filePath)));
    EXPECT_EQ(model.contract.kind, "anomaly-autoencoder");
    EXPECT_EQ(model.contract.inputWidth, 64);
    EXPECT_EQ(model.contract.numbers.at("threshold"), 0.021);
    EXPECT_EQ(model.provenance.kind, "trained");
    EXPECT_EQ(model.provenance.trainingJobId, job.value());
    EXPECT_EQ(model.provenance.datasetVersionId, dataset);
    EXPECT_EQ(model.provenance.split, "0.8/0.1/0.1 seed=1");
    EXPECT_EQ(model.provenance.backend, "scripted");

    EXPECT_EQ(p->count("TrainingJobSubmitted"), 1);
    EXPECT_EQ(p->count("TrainingJobStarted"), 1);
    EXPECT_EQ(p->count("TrainingJobCompleted"), 1);
    EXPECT_EQ(p->count("ModelVersionImported"), 1);

    // T-4：结束之后再刷新，不再去问训练后端，什么都不变
    const auto pollsBefore = backend.polled.size();
    EXPECT_EQ(p->training.refresh(job.value()).value().status, "completed");
    EXPECT_EQ(backend.polled.size(), pollsBefore);
    EXPECT_EQ(p->models.listModelVersions().size(), 1u);
    EXPECT_FALSE(p->training.cancel(job.value()).ok());

    // 发布是人的决定
    ASSERT_TRUE(p->models.publish("surface@v1").ok());
    EXPECT_EQ(p->models.getModelVersion("surface@v1").value().status, "published");
}

// 同一个模型名再训练一次：得到下一个版本，前一个不变
TEST_F(TrainingLoop, TrainingAgainGivesTheNextVersion) {
    const auto dataset = frozenDataset();
    for (int i = 1; i <= 2; ++i) {
        const auto job = p->training.submit(request(dataset)).value();
        backend.completed(modelFile("model-bytes-" + std::to_string(i)));
        EXPECT_EQ(p->training.refresh(job).value().modelVersionId, "surface@v" + std::to_string(i));
    }
    EXPECT_NE(p->models.getModelVersion("surface@v1").value().sha256, p->models.getModelVersion("surface@v2").value().sha256);
}

// 通过标准：训练服务不可用时，任务状态正确，不影响方案构建平台的其他功能
TEST_F(TrainingLoop, UnavailableBackendLeavesAFailedJobAndEverythingElseWorks) {
    const auto dataset = frozenDataset();
    backend.available = false;
    const auto job = p->training.submit(request(dataset));
    ASSERT_TRUE(job.ok());  // 任务是合规的，只是没能开始：有记录
    const auto view = p->training.getJob(job.value()).value();
    EXPECT_EQ(view.status, "failed");
    EXPECT_NE(view.failure.find("训练后端不可用"), std::string::npos);
    EXPECT_NE(view.failure.find("训练服务没有启动"), std::string::npos);
    EXPECT_EQ(p->count("TrainingJobFailed"), 1);
    EXPECT_TRUE(p->models.listModelVersions().empty());

    // 其他功能照常：导入样本、建数据集
    EXPECT_TRUE(p->data.importSample(image("later.png", 999), "ok").ok());
    EXPECT_TRUE(p->data.createDataset("另一个").ok());
    // 训练服务恢复之后，重新提交一次就行（T-4：失败的任务不会“复活”）
    backend.available = true;
    const auto retry = p->training.submit(request(dataset)).value();
    EXPECT_EQ(p->training.getJob(retry).value().status, "running");
    EXPECT_EQ(p->training.getJob(job.value()).value().status, "failed");
}

// 通过标准：训练失败时，任务状态正确
TEST_F(TrainingLoop, TrainingFailureIsRecordedWithItsReason) {
    const auto job = p->training.submit(request(frozenDataset())).value();
    backend.running(0.2);
    backend.failed("内存不足");
    EXPECT_EQ(p->training.refresh(job).value().status, "running");
    const auto view = p->training.refresh(job).value();
    EXPECT_EQ(view.status, "failed");
    EXPECT_EQ(view.failure, "内存不足");
    EXPECT_TRUE(p->models.listModelVersions().empty());
}

// 查询进度时训练后端暂时连不上：任务的状态不变（训练可能还在进行），下次再问
TEST_F(TrainingLoop, TemporaryPollFailureDoesNotFailTheJob) {
    const auto job = p->training.submit(request(frozenDataset())).value();
    EXPECT_EQ(p->training.refresh(job).value().status, "running");  // 脚本是空的：这一次“查询超时”
    backend.completed(modelFile());
    EXPECT_EQ(p->training.refresh(job).value().status, "completed");
}

TEST_F(TrainingLoop, CancelTellsTheBackendAndIgnoresLateResults) {
    const auto job = p->training.submit(request(frozenDataset())).value();
    ASSERT_TRUE(p->training.cancel(job).ok());
    EXPECT_EQ(backend.cancelled, std::vector<std::string>{"ticket-1"});
    EXPECT_EQ(p->training.getJob(job).value().status, "cancelled");
    backend.completed(modelFile());  // 训练后端后来还是报告了完成
    EXPECT_EQ(p->training.refresh(job).value().status, "cancelled");
    EXPECT_TRUE(p->models.listModelVersions().empty());
    EXPECT_FALSE(p->training.cancel(job).ok());
}

// 不合规则的提交：直接拒绝，不产生任务，也不打扰训练后端
TEST_F(TrainingLoop, InvalidSubmissionsCreateNoJob) {
    EXPECT_EQ(p->training.submit(request("no-such@v1")).error().code, "data.version_not_found");
    EXPECT_EQ(p->training.submit(request(frozenDataset(5, 2))).error().code, "training.too_few_samples");  // 合格的太少
    auto badSplit = request(frozenDataset());
    badSplit.train = 0.5;
    EXPECT_EQ(p->training.submit(badSplit).error().code, "training.bad_split");
    auto noName = request(frozenDataset(20, 0));
    noName.modelName.clear();
    EXPECT_EQ(p->training.submit(noName).error().code, "training.bad_config");
    EXPECT_TRUE(p->training.listJobs().empty());
    EXPECT_TRUE(backend.requests.empty());
    EXPECT_FALSE(p->training.refresh("no-such-job").ok());
}

// 训练后端报告完成，但结果不完整（没有模型文件）：任务记为失败，而不是一个没有模型的“已完成”
TEST_F(TrainingLoop, IncompleteResultFromBackendFailsTheJob) {
    const auto job = p->training.submit(request(frozenDataset())).value();
    backend.completed("");
    const auto view = p->training.refresh(job).value();
    EXPECT_EQ(view.status, "failed");
    EXPECT_NE(view.failure.find("结果不完整"), std::string::npos);
}

// 训练成功但模型文件读不到（导入失败）：任务仍然是“已完成”，只是没有模型版本；不会崩溃
TEST_F(TrainingLoop, ImportFailureKeepsTheJobCompleted) {
    const auto job = p->training.submit(request(frozenDataset())).value();
    backend.completed(pathToUtf8(dir / "does-not-exist.onnx"));
    const auto view = p->training.refresh(job).value();
    EXPECT_EQ(view.status, "completed");
    EXPECT_TRUE(view.modelVersionId.empty());
    EXPECT_TRUE(p->models.listModelVersions().empty());
}

// T-2 的最后一道防线：数据上下文的 N1 让它不会发生，但训练上下文自己还是要检查。
// 这里用一个假的数据集来源，交出一个“含有留作基准的样本”的数据集版本。
TEST_F(TrainingLoop, T2_SubmissionRechecksBenchmarkSamples) {
    class LeakySource final : public training::DatasetSource {
    public:
        Result<training::DatasetInfo> load(const std::string& id) override {
            training::DatasetInfo info;
            info.datasetVersionId = id;
            info.frozen = true;
            for (int i = 0; i < 12; ++i) info.samples.push_back({"s" + std::to_string(i), false, i == 3, "/img"});  // 第 4 个是留作基准的
            return info;
        }
    } leaky;
    training::TrainingService service(p->jobs, backend, leaky, p->importer, p->bus);
    const auto rejected = service.submit(request("ds@v1"));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "training.n1");
    EXPECT_TRUE(backend.requests.empty());
}

// 存盘再读回来：进行中的任务可以接着查询；完成的任务和模型版本都还在
TEST_F(TrainingLoop, SurvivesRestart) {
    const auto dataset = frozenDataset();
    const auto job = p->training.submit(request(dataset)).value();
    p = std::make_unique<Platform>(dir, backend);  // “重启”
    EXPECT_EQ(p->training.getJob(job).value().status, "running");
    backend.completed(modelFile());
    EXPECT_EQ(p->training.refresh(job).value().modelVersionId, "surface@v1");
    EXPECT_EQ(backend.polled.back(), "ticket-1");  // 用的还是原来的编号

    p = std::make_unique<Platform>(dir, backend);
    const auto view = p->training.getJob(job).value();
    EXPECT_EQ(view.status, "completed");
    EXPECT_EQ(view.metrics.at("val_loss"), 0.004);
    EXPECT_EQ(view.split, "0.8/0.1/0.1 seed=1");
    EXPECT_EQ(p->models.getModelVersion("surface@v1").value().provenance.datasetVersionId, dataset);
}

// 没有配置训练后端的机器：提交的任务以“已失败”记录，原因说得清
TEST_F(TrainingLoop, NoBackendConfigured) {
    content::NoTrainingBackend none;
    training::TrainingService service(p->jobs, none, p->datasetSource, p->importer, p->bus);
    const auto job = service.submit(request(frozenDataset())).value();
    const auto view = service.getJob(job).value();
    EXPECT_EQ(view.status, "failed");
    EXPECT_NE(view.failure.find("没有配置训练服务"), std::string::npos);
}
