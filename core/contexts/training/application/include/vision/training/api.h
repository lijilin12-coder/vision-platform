#pragma once

// BC-12 模型训练的公开接口。
//
// 对方案设计这一侧来说，训练只是“交出去一个数据集版本，拿回来一个模型版本”。
// 训练上下文不直接依赖数据和模型仓库：需要的能力写成下面两个端口，由组装根接上。

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::training {

class TrainingJobRepository;  // 领域层
class TrainingBackend;        // 领域层：训练后端（ADR-30）

// ===== 端口 =====

struct DatasetSampleInfo {
    std::string sampleId;
    bool ng = false;
    bool benchmark = false;   // 现在是不是留作基准的（T-2：提交时再检查一次）
    std::string contentPath;  // 图像文件的位置
};

struct DatasetInfo {
    std::string datasetVersionId;
    bool frozen = false;
    std::vector<DatasetSampleInfo> samples;
};

// 数据集版本从哪里来（接到 BC-9 数据上）
class DatasetSource {
public:
    virtual ~DatasetSource() = default;
    virtual Result<DatasetInfo> load(const std::string& datasetVersionId) = 0;
};

struct TrainedModel {
    std::string modelName;
    std::string modelFile;
    std::string contractKind;
    int inputWidth = 0, inputHeight = 0, inputChannels = 1;
    std::string outputMeaning;
    std::map<std::string, double> numbers;
    // 来源（T-5）
    std::string trainingJobId, datasetVersionId, split, backend;
};

// 训练出来的模型交到哪里去（接到 BC-7 模型仓库上）。返回模型版本 ID。
class ModelImporter {
public:
    virtual ~ModelImporter() = default;
    virtual Result<std::string> importTrained(const TrainedModel& model) = 0;
};

// ===== 应用服务 =====

struct SubmitRequest {
    std::string datasetVersionId;
    std::string modelKind = "anomaly-autoencoder";
    std::string modelName;                   // 训练出来的模型叫什么
    std::map<std::string, double> settings;  // 交给训练后端的设置，例如 epochs
    double train = 0.8, validation = 0.1, test = 0.1;
    unsigned seed = 1;
};

struct JobView {
    std::string id;
    std::string datasetVersionId;
    std::string modelKind, modelName;
    std::string split;    // "0.8/0.1/0.1 seed=1"
    std::string backend;
    std::string status;   // submitted / running / completed / failed / cancelled
    std::string failure;  // 失败的原因
    double progress = 0;  // 最近一次查询到的进度，0～1
    std::map<std::string, double> metrics;  // 训练报告里的指标
    double seconds = 0;
    std::string modelVersionId;  // 完成并导入模型仓库之后才有
};

class TrainingService {
public:
    TrainingService(TrainingJobRepository& jobs, TrainingBackend& backend, DatasetSource& datasets, ModelImporter& models,
                    EventPublisher& events);

    // 提交训练任务。数据集版本不合规则（T-1、T-2、样本太少）时返回失败，不产生任务。
    // 规则都通过、但训练后端连不上时：任务照样记录下来，状态是“已失败”，原因写明——返回的是任务 ID。
    Result<std::string> submit(const SubmitRequest& request);

    // 向训练后端查询进度，更新任务。训练完成时把模型导入模型仓库（未发布），记下模型版本。
    Result<JobView> refresh(const std::string& jobId);
    Status cancel(const std::string& jobId);

    Result<JobView> getJob(const std::string& jobId) const;
    std::vector<std::string> listJobs() const;

private:
    TrainingJobRepository& jobs_;
    TrainingBackend& backend_;
    DatasetSource& datasets_;
    ModelImporter& models_;
    EventPublisher& events_;
    mutable std::mutex mutex_;
    std::map<std::string, double> progress_;  // 进度只在内存里：它不是任务的一部分，只是最近一次查到的数字
};

}  // namespace vision::training
