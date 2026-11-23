#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::models {

// 值对象：模型契约。模型的输入输出规格：使用模型的算子靠它知道该怎么喂图像、怎么读结果。
// 契约变化 = 主版本号变化（术语表 §7）。
struct ModelContract {
    std::string kind;        // 这是哪一类模型，例如 "anomaly-autoencoder"
    int inputWidth = 0;      // 输入图像的尺寸和通道数
    int inputHeight = 0;
    int inputChannels = 1;
    std::string output;      // 输出的含义，例如 "reconstruction"（重建图像）
    // 使用时需要的其他数字，例如异常得分的建议阈值、归一化用的均值
    std::map<std::string, double> numbers;
};

// 值对象：来源。模型版本是怎么来的（T-5：训练出来的模型必须记录来源）。
struct Provenance {
    std::string kind;              // "trained"（训练任务产出）/ "imported"（手工导入）
    std::string trainingJobId;     // 训练出来的才有下面这些
    std::string datasetVersionId;
    std::string split;             // 划分方式的文字形式，例如 "0.8/0.1/0.1 seed=1"
    std::string backend;           // 哪个训练后端
};

enum class ModelStatus { Unpublished, Published };

// 聚合根：模型版本。ID 形如 "<模型名>@v<N>"。
//
// 不变量：
//   - 内容（模型文件、契约、来源）创建后不可修改
//   - 状态只能从“未发布”变为“已发布”
//   - 训练出来的模型版本必须有完整的来源：训练任务、数据集版本、划分方式（T-5）
class ModelVersion : public EventRecorder {
public:
    static Result<ModelVersion> create(std::string modelName, int number, std::string contentHash, long long bytes,
                                       ModelContract contract, Provenance provenance);
    static ModelVersion restore(std::string modelName, int number, std::string contentHash, long long bytes, ModelContract contract,
                                Provenance provenance, ModelStatus status);

    Status publish();

    std::string id() const { return modelName_ + "@v" + std::to_string(number_); }
    const std::string& modelName() const { return modelName_; }
    int number() const { return number_; }
    const std::string& contentHash() const { return contentHash_; }
    long long bytes() const { return bytes_; }
    const ModelContract& contract() const { return contract_; }
    const Provenance& provenance() const { return provenance_; }
    ModelStatus status() const { return status_; }

private:
    ModelVersion() = default;
    std::string modelName_;
    int number_ = 0;
    std::string contentHash_;  // 模型文件（ONNX）内容的哈希，文件本身在内容仓库里
    long long bytes_ = 0;
    ModelContract contract_;
    Provenance provenance_;
    ModelStatus status_ = ModelStatus::Unpublished;
};

struct ModelVersionImported final : DomainEvent {
    std::string modelVersionId;
    explicit ModelVersionImported(std::string id) : modelVersionId(std::move(id)) {}
    const char* name() const override { return "ModelVersionImported"; }
};

struct ModelVersionPublished final : DomainEvent {
    std::string modelVersionId;
    explicit ModelVersionPublished(std::string id) : modelVersionId(std::move(id)) {}
    const char* name() const override { return "ModelVersionPublished"; }
};

class ModelVersionRepository {
public:
    virtual ~ModelVersionRepository() = default;
    virtual Status save(const ModelVersion& version) = 0;
    virtual std::optional<ModelVersion> find(const std::string& versionId) = 0;
    virtual std::vector<std::string> listIds() = 0;
};

}  // namespace vision::models
