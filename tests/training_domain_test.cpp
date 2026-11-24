// BC-12 模型训练、BC-7 模型仓库领域层的单元测试（POC-13 第 2 步）。只链接领域库。
// 测试名字里的 T-1…T-5 对应 docs/ddd/08-platforms-and-training.md §2.4 的不变量。
#include <gtest/gtest.h>

#include <algorithm>

#include "vision/models/domain/model_version.h"
#include "vision/training/domain/training_job.h"

using namespace vision;
using namespace vision::training;

namespace {

DatasetFacts goodDataset() { return {"ds-1@v1", true, 20, 4, 0}; }
TrainingConfig config() { return {"anomaly-autoencoder", "surface", {{"epochs", 5}}}; }

TrainingJob submitted() {
    auto job = TrainingJob::submit("job-1", goodDataset(), config(), SplitSpec{}, "fake");
    EXPECT_TRUE(job.ok());
    return std::move(job).value();
}

TrainingOutput output() { return {"/tmp/model.onnx", "anomaly-autoencoder", 64, 64, 1, "reconstruction", {{"threshold", 0.02}}}; }

std::vector<std::string> names(TrainingJob& job) {
    std::vector<std::string> out;
    for (const auto& e : job.pullEvents()) out.emplace_back(e->name());
    return out;
}

}  // namespace

TEST(TrainingJob, SubmitRecordsEverythingNeededToReproduceIt) {
    TrainingJob job = submitted();
    EXPECT_EQ(job.status(), JobStatus::Submitted);
    EXPECT_EQ(job.state().datasetVersionId, "ds-1@v1");
    EXPECT_EQ(job.state().split.text(), "0.8/0.1/0.1 seed=1");  // T-3：划分方式记录在案
    EXPECT_EQ(job.state().backend, "fake");
    EXPECT_EQ(names(job), std::vector<std::string>{"TrainingJobSubmitted"});
}

// T-1：只能用已冻结的数据集版本
TEST(TrainingJob, T1_RequiresFrozenDatasetVersion) {
    DatasetFacts draft = goodDataset();
    draft.frozen = false;
    EXPECT_EQ(TrainingJob::submit("j", draft, config(), {}, "b").error().code, "training.dataset_not_frozen");
    DatasetFacts none = goodDataset();
    none.datasetVersionId.clear();
    EXPECT_EQ(TrainingJob::submit("j", none, config(), {}, "b").error().code, "training.no_dataset");
}

// T-2：数据集版本里不能有留作基准的样本（规则 N1 在提交时再检查一次）
TEST(TrainingJob, T2_RejectsBenchmarkSamples) {
    DatasetFacts leaked = goodDataset();
    leaked.benchmarkSamples = 1;
    const auto rejected = TrainingJob::submit("j", leaked, config(), {}, "b");
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, "training.n1");
}

// T-3：划分方式必须合法
TEST(TrainingJob, T3_SplitMustBeValid) {
    EXPECT_TRUE(SplitSpec{}.validate().ok());
    EXPECT_TRUE((SplitSpec{1.0, 0.0, 0.0, 7}).validate().ok());  // 全部用来训练是允许的
    EXPECT_FALSE((SplitSpec{0.5, 0.2, 0.2, 1}).validate().ok());  // 加起来不是 1
    EXPECT_FALSE((SplitSpec{0.0, 0.5, 0.5, 1}).validate().ok());  // 没有训练集
    EXPECT_FALSE((SplitSpec{1.2, -0.1, -0.1, 1}).validate().ok());
    EXPECT_EQ(TrainingJob::submit("j", goodDataset(), config(), SplitSpec{0.5, 0.2, 0.2, 1}, "b").error().code, "training.bad_split");
}

TEST(TrainingJob, RejectsTooFewOkSamplesAndIncompleteConfig) {
    DatasetFacts few = goodDataset();
    few.okCount = TrainingJob::kMinOkSamples - 1;
    EXPECT_EQ(TrainingJob::submit("j", few, config(), {}, "b").error().code, "training.too_few_samples");
    EXPECT_EQ(TrainingJob::submit("j", goodDataset(), {"", "name", {}}, {}, "b").error().code, "training.bad_config");
    EXPECT_EQ(TrainingJob::submit("j", goodDataset(), {"kind", "", {}}, {}, "b").error().code, "training.bad_config");
    EXPECT_FALSE(TrainingJob::submit("", goodDataset(), config(), {}, "b").ok());
    // 没有不合格的样本是允许的：异常检测模型只靠合格的样本训练
    DatasetFacts onlyOk = goodDataset();
    onlyOk.ngCount = 0;
    EXPECT_TRUE(TrainingJob::submit("j", onlyOk, config(), {}, "b").ok());
}

TEST(TrainingJob, HappyPath) {
    TrainingJob job = submitted();
    (void)job.pullEvents();
    ASSERT_TRUE(job.markStarted("ticket-9").ok());
    EXPECT_EQ(job.status(), JobStatus::Running);
    EXPECT_EQ(job.state().backendTicket, "ticket-9");
    ASSERT_TRUE(job.complete({{{"test_auroc", 0.97}}, 12.5, ""}, output()).ok());
    EXPECT_EQ(job.status(), JobStatus::Completed);
    EXPECT_EQ(job.state().report->metrics.at("test_auroc"), 0.97);
    EXPECT_EQ(names(job), (std::vector<std::string>{"TrainingJobStarted", "TrainingJobCompleted"}));

    ASSERT_TRUE(job.recordModelVersion("surface@v1").ok());
    EXPECT_FALSE(job.recordModelVersion("surface@v2").ok());  // 只记录一次
}

// T-4：已经结束的任务不可修改
TEST(TrainingJob, T4_FinishedJobsCannotChange) {
    TrainingJob completed = submitted();
    ASSERT_TRUE(completed.markStarted("t").ok());
    ASSERT_TRUE(completed.complete({}, output()).ok());
    EXPECT_EQ(completed.fail("x").error().code, "training.finished");
    EXPECT_EQ(completed.cancel().error().code, "training.finished");
    EXPECT_EQ(completed.complete({}, output()).error().code, "training.finished");
    EXPECT_EQ(completed.markStarted("t2").error().code, "training.finished");
    EXPECT_EQ(completed.status(), JobStatus::Completed);

    TrainingJob failed = submitted();
    ASSERT_TRUE(failed.fail("训练后端不可用").ok());  // 还没开始就失败（连不上训练后端）
    EXPECT_EQ(failed.state().failure, "训练后端不可用");
    EXPECT_FALSE(failed.markStarted("t").ok());
    EXPECT_FALSE(failed.recordModelVersion("m@v1").ok());  // 没完成的任务没有模型版本

    TrainingJob cancelled = submitted();
    ASSERT_TRUE(cancelled.markStarted("t").ok());
    ASSERT_TRUE(cancelled.cancel().ok());
    EXPECT_FALSE(cancelled.complete({}, output()).ok());  // 取消之后训练后端才报告完成：不接受
    EXPECT_EQ(cancelled.status(), JobStatus::Cancelled);
}

TEST(TrainingJob, CompletionRequiresAModelAndItsContract) {
    TrainingJob job = submitted();
    EXPECT_EQ(job.complete({}, output()).error().code, "training.not_running");  // 还没开始
    ASSERT_TRUE(job.markStarted("t").ok());
    EXPECT_FALSE(job.markStarted("t").ok());
    TrainingOutput noFile = output();
    noFile.modelFile.clear();
    EXPECT_EQ(job.complete({}, noFile).error().code, "training.no_output");
    TrainingOutput noContract = output();
    noContract.inputWidth = 0;
    EXPECT_EQ(job.complete({}, noContract).error().code, "training.no_contract");
    EXPECT_EQ(job.status(), JobStatus::Running);  // 被拒绝的完成不改变状态
}

// T-5：来源信息——训练任务、数据集版本、划分方式
TEST(TrainingJob, T5_ProvenanceIsCompleteAndModelVersionRequiresIt) {
    TrainingJob job = submitted();
    const auto p = job.provenance();
    EXPECT_EQ(p.trainingJobId, "job-1");
    EXPECT_EQ(p.datasetVersionId, "ds-1@v1");
    EXPECT_EQ(p.split, "0.8/0.1/0.1 seed=1");
    EXPECT_EQ(p.backend, "fake");

    using namespace vision::models;
    const ModelContract contract{"anomaly-autoencoder", 64, 64, 1, "reconstruction", {}};
    EXPECT_TRUE(ModelVersion::create("surface", 1, "hash", 100, contract, {"trained", p.trainingJobId, p.datasetVersionId, p.split, p.backend}).ok());
    // 训练出来的模型缺了任何一项来源，都不能进模型仓库
    EXPECT_FALSE(ModelVersion::create("surface", 1, "hash", 100, contract, {"trained", "", p.datasetVersionId, p.split, ""}).ok());
    EXPECT_FALSE(ModelVersion::create("surface", 1, "hash", 100, contract, {"trained", p.trainingJobId, "", p.split, ""}).ok());
    EXPECT_FALSE(ModelVersion::create("surface", 1, "hash", 100, contract, {"trained", p.trainingJobId, p.datasetVersionId, "", ""}).ok());
    // 手工导入的模型不需要这些
    EXPECT_TRUE(ModelVersion::create("surface", 1, "hash", 100, contract, {"imported", "", "", "", ""}).ok());
}

// T-3（续）：同样的样本和划分方式，划分结果相同，而且和样本的顺序无关
TEST(Split, DeterministicStratifiedAndOrderIndependent) {
    std::vector<std::pair<std::string, bool>> samples;
    for (int i = 0; i < 40; ++i) samples.emplace_back("ok" + std::to_string(i), false);
    for (int i = 0; i < 10; ++i) samples.emplace_back("ng" + std::to_string(i), true);
    const SplitSpec spec{0.6, 0.2, 0.2, 5};
    const auto splits = assignSplits(samples, spec);
    ASSERT_EQ(splits.size(), samples.size());

    const auto count = [&](bool ng, Split which) {
        int n = 0;
        for (std::size_t i = 0; i < samples.size(); ++i) n += samples[i].second == ng && splits[i] == which;
        return n;
    };
    EXPECT_EQ(count(false, Split::Train), 24);  // 合格的 40 个：24 / 8 / 8
    EXPECT_EQ(count(false, Split::Validation), 8);
    EXPECT_EQ(count(false, Split::Test), 8);
    EXPECT_EQ(count(true, Split::Train), 6);    // 不合格的 10 个：6 / 2 / 2（两类各自按比例分）
    EXPECT_EQ(count(true, Split::Test), 2);

    EXPECT_EQ(assignSplits(samples, spec), splits);  // 再来一次：一样
    auto reversed = samples;
    std::reverse(reversed.begin(), reversed.end());
    auto reversedSplits = assignSplits(reversed, spec);
    std::reverse(reversedSplits.begin(), reversedSplits.end());
    EXPECT_EQ(reversedSplits, splits);  // 和输入的顺序无关

    EXPECT_NE(assignSplits(samples, SplitSpec{0.6, 0.2, 0.2, 6}), splits);  // 换种子，换一种分法
    // 全部用来训练
    const auto all = assignSplits(samples, SplitSpec{1.0, 0.0, 0.0, 1});
    EXPECT_TRUE(std::all_of(all.begin(), all.end(), [](Split s) { return s == Split::Train; }));
}

// ---------------- 模型版本 ----------------

TEST(ModelVersion, CreateValidatesAndPublishIsOneWay) {
    using namespace vision::models;
    const ModelContract contract{"anomaly-autoencoder", 64, 64, 1, "reconstruction", {{"threshold", 0.02}}};
    const Provenance imported{"imported", "", "", "", ""};
    auto v = ModelVersion::create("surface", 3, "hash", 100, contract, imported);
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value().id(), "surface@v3");
    EXPECT_EQ(v.value().status(), ModelStatus::Unpublished);  // 导入的模型一开始是未发布的
    ASSERT_EQ(v.value().pullEvents().size(), 1u);

    ASSERT_TRUE(v.value().publish().ok());
    EXPECT_EQ(v.value().status(), ModelStatus::Published);
    EXPECT_FALSE(v.value().publish().ok());

    EXPECT_FALSE(ModelVersion::create("", 1, "hash", 100, contract, imported).ok());
    EXPECT_FALSE(ModelVersion::create("a@b", 1, "hash", 100, contract, imported).ok());  // 名字里不能有 @
    EXPECT_FALSE(ModelVersion::create("m", 0, "hash", 100, contract, imported).ok());
    EXPECT_FALSE(ModelVersion::create("m", 1, "", 100, contract, imported).ok());
    EXPECT_FALSE(ModelVersion::create("m", 1, "hash", 0, contract, imported).ok());
    EXPECT_FALSE(ModelVersion::create("m", 1, "hash", 100, {"", 64, 64, 1, "", {}}, imported).ok());
    EXPECT_FALSE(ModelVersion::create("m", 1, "hash", 100, {"k", 0, 64, 1, "", {}}, imported).ok());
    EXPECT_FALSE(ModelVersion::create("m", 1, "hash", 100, contract, {"guessed", "", "", "", ""}).ok());
}
