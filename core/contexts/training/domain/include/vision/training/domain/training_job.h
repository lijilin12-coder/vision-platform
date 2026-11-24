#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::training {

using TrainingJobId = std::string;

// 值对象：训练配置。训练哪一种模型、关键的设置。（不叫“配方”：那是“方案”的禁用近义词。）
struct TrainingConfig {
    std::string modelKind;                   // 例如 "anomaly-autoencoder"
    std::string modelName;                   // 训练出来的模型在模型仓库里叫什么
    std::map<std::string, double> settings;  // 例如 epochs、imageSize
};

// 值对象：划分方式。训练集、验证集、测试集的比例和随机种子（T-3：必须记录，保证可重现）。
// “训练集、验证集、测试集”这三个词只属于训练上下文。
struct SplitSpec {
    double train = 0.8;
    double validation = 0.1;
    double test = 0.1;
    unsigned seed = 1;

    Status validate() const;
    std::string text() const;  // "0.8/0.1/0.1 seed=1"
};

enum class Split { Train, Validation, Test };
const char* toString(Split split);

// 按划分方式把样本分成三部分。合格、不合格两类各自按比例分（分层），
// 同样的样本和划分方式，在任何平台上结果都相同。
// samples：样本 ID 和它是否不合格。返回的顺序和输入一致。
std::vector<Split> assignSplits(const std::vector<std::pair<std::string, bool>>& samples, const SplitSpec& spec);

// 提交训练任务时，数据集版本的情况
struct DatasetFacts {
    std::string datasetVersionId;
    bool frozen = false;       // T-1
    int okCount = 0;
    int ngCount = 0;
    int benchmarkSamples = 0;  // 其中留作基准的样本数（T-2：必须是 0）
};

// 值对象：训练报告。训练过程和结果（模型在验证集、测试集上的指标）。
struct TrainingReport {
    std::map<std::string, double> metrics;  // 例如 test_auroc、val_loss
    double seconds = 0;
    std::string notes;
};

// 值对象：训练的产出
struct TrainingOutput {
    std::string modelFile;  // 模型文件（ONNX）的位置
    std::string contractKind;
    int inputWidth = 0, inputHeight = 0, inputChannels = 1;
    std::string outputMeaning;
    std::map<std::string, double> numbers;
};

enum class JobStatus { Submitted, Running, Completed, Failed, Cancelled };
const char* toString(JobStatus status);

// 快照：聚合的全部状态，保存和恢复用
struct JobState {
    TrainingJobId id;
    std::string datasetVersionId;
    TrainingConfig config;
    SplitSpec split;
    std::string backend;        // 交给了哪个训练后端（只作记录）
    std::string backendTicket;  // 训练后端给这次训练的编号
    JobStatus status = JobStatus::Submitted;
    std::string failure;        // 失败的原因
    std::optional<TrainingReport> report;
    std::optional<TrainingOutput> output;
    std::string modelVersionId;  // 导入模型仓库之后得到的模型版本
};

// 聚合根：训练任务。用一个数据集版本、按一份训练配置训练一个模型的一次执行
// （docs/ddd/08-platforms-and-training.md §2.4）。
//
// 不变量：
//   T-1  只能使用已冻结的数据集版本
//   T-2  数据集版本里不能有任何留作基准的样本（规则 N1 在提交时再检查一次）
//   T-3  划分方式必须合法，并且记录下来
//   T-4  已经结束（完成、失败、取消）的任务不可修改。想换设置，就提交新的任务
//   T-5  产出的模型版本必须记录来源：训练任务、数据集版本、划分方式（provenance() 给出）
// 另外：合格的样本太少时拒绝提交（异常检测模型靠合格的样本学习“正常是什么样”）。
class TrainingJob : public EventRecorder {
public:
    static constexpr int kMinOkSamples = 8;

    static Result<TrainingJob> submit(TrainingJobId id, const DatasetFacts& dataset, TrainingConfig config, SplitSpec split,
                                      std::string backend);
    static TrainingJob restore(JobState state);

    Status markStarted(std::string backendTicket);
    Status complete(TrainingReport report, TrainingOutput output);
    Status fail(const std::string& reason);
    Status cancel();
    // 模型已经导入模型仓库：记下模型版本。只能在完成之后记录一次。
    Status recordModelVersion(const std::string& modelVersionId);

    // T-5：这次训练的来源信息
    struct ProvenanceInfo {
        std::string trainingJobId, datasetVersionId, split, backend;
    };
    ProvenanceInfo provenance() const { return {s_.id, s_.datasetVersionId, s_.split.text(), s_.backend}; }

    bool finished() const { return s_.status == JobStatus::Completed || s_.status == JobStatus::Failed || s_.status == JobStatus::Cancelled; }
    const JobState& state() const { return s_; }
    const TrainingJobId& id() const { return s_.id; }
    JobStatus status() const { return s_.status; }

private:
    explicit TrainingJob(JobState state) : s_(std::move(state)) {}
    Status requireNotFinished() const;
    JobState s_;
};

// ---- 领域事件（08 §2.3）----
struct TrainingJobSubmitted final : DomainEvent {
    std::string jobId, datasetVersionId;
    TrainingJobSubmitted(std::string j, std::string d) : jobId(std::move(j)), datasetVersionId(std::move(d)) {}
    const char* name() const override { return "TrainingJobSubmitted"; }
};
struct TrainingJobStarted final : DomainEvent {
    std::string jobId;
    explicit TrainingJobStarted(std::string j) : jobId(std::move(j)) {}
    const char* name() const override { return "TrainingJobStarted"; }
};
struct TrainingJobCompleted final : DomainEvent {
    std::string jobId, modelFile;
    TrainingJobCompleted(std::string j, std::string f) : jobId(std::move(j)), modelFile(std::move(f)) {}
    const char* name() const override { return "TrainingJobCompleted"; }
};
struct TrainingJobFailed final : DomainEvent {
    std::string jobId, reason;
    TrainingJobFailed(std::string j, std::string r) : jobId(std::move(j)), reason(std::move(r)) {}
    const char* name() const override { return "TrainingJobFailed"; }
};
struct TrainingJobCancelled final : DomainEvent {
    std::string jobId;
    explicit TrainingJobCancelled(std::string j) : jobId(std::move(j)) {}
    const char* name() const override { return "TrainingJobCancelled"; }
};

class TrainingJobRepository {
public:
    virtual ~TrainingJobRepository() = default;
    virtual Status save(const TrainingJob& job) = 0;
    virtual std::optional<TrainingJob> find(const TrainingJobId& id) = 0;
    virtual std::vector<TrainingJobId> listIds() = 0;
};

// ---- 端口：训练后端（ADR-30）----
// 真正执行训练的地方。可以是我们自己的训练服务，也可以是外部的训练平台；领域层不知道是哪一个。

struct TrainingSample {
    std::string sampleId;
    bool ng = false;
    Split split = Split::Train;
    std::string contentPath;  // 图像文件的位置
};

struct TrainingRequest {
    TrainingJobId jobId;
    TrainingConfig config;
    std::vector<TrainingSample> samples;
};

struct BackendProgress {
    enum class State { Running, Completed, Failed };
    State state = State::Running;
    double progress = 0;  // 0～1
    std::string failure;
    TrainingReport report;   // 完成时才有
    TrainingOutput output;   // 完成时才有
};

class TrainingBackend {
public:
    virtual ~TrainingBackend() = default;
    virtual std::string name() const = 0;
    // 开始训练，返回训练后端给这次训练的编号。连不上训练后端时返回失败。
    virtual Result<std::string> start(const TrainingRequest& request) = 0;
    virtual Result<BackendProgress> poll(const std::string& ticket) = 0;
    virtual Status cancel(const std::string& ticket) = 0;
};

}  // namespace vision::training
