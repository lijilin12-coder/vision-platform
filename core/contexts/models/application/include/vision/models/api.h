#pragma once

// BC-7 模型仓库的公开接口（训练闭环需要的最小部分）。

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::models {

class ModelVersionRepository;  // 领域层

// 端口：保存模型文件。把文件原样存进内容仓库，返回内容的哈希（ADR-20）。
struct StoredModelFile {
    std::string sha256;
    long long bytes = 0;
};

class ModelFiles {
public:
    virtual ~ModelFiles() = default;
    virtual Result<StoredModelFile> importFile(const std::string& path) = 0;
    // 模型文件的内容已经在内存里时（从方案包里带来的）
    virtual Result<StoredModelFile> importBytes(const std::vector<unsigned char>& bytes) = 0;
    virtual std::string pathOf(const std::string& sha256) const = 0;
};

struct ContractView {
    std::string kind;
    int inputWidth = 0, inputHeight = 0, inputChannels = 1;
    std::string output;
    std::map<std::string, double> numbers;
};

struct ProvenanceView {
    std::string kind;  // trained / imported
    std::string trainingJobId, datasetVersionId, split, backend;
};

struct ModelVersionView {
    std::string id;  // "<模型名>@v<N>"
    std::string modelName;
    std::string status;  // unpublished / published
    std::string sha256;
    long long bytes = 0;
    std::string filePath;  // 模型文件的位置
    ContractView contract;
    ProvenanceView provenance;
};

class ModelService {
public:
    ModelService(ModelVersionRepository& versions, ModelFiles& files, EventPublisher& events);

    // 导入一个模型文件，成为 modelName 的下一个版本（未发布）。返回模型版本 ID。
    Result<std::string> importModel(const std::string& modelName, const std::string& filePath, const ContractView& contract,
                                    const ProvenanceView& provenance);
    Status publish(const std::string& modelVersionId);
    // 接收一个在别的机器上已经发布的模型版本（从方案包里带来的）：ID 不变，状态直接是“已发布”。
    //   本机没有这个 ID      → 登记
    //   有，内容一样         → 什么都不做（重复接收不出错）
    //   有，内容不一样       → 拒绝：模型版本是不可变的，同一个 ID 不能指两样东西
    Status adoptModel(const std::string& modelVersionId, const std::vector<unsigned char>& bytes, const ContractView& contract,
                      const ProvenanceView& provenance);
    Result<ModelVersionView> getModelVersion(const std::string& modelVersionId) const;
    std::vector<std::string> listModelVersions() const;

private:
    ModelVersionRepository& versions_;
    ModelFiles& files_;
    EventPublisher& events_;
    mutable std::mutex mutex_;
};

}  // namespace vision::models
