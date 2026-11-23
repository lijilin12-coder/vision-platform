#include "vision/models/domain/model_version.h"

#include <memory>

namespace vision::models {

Result<ModelVersion> ModelVersion::create(std::string modelName, int number, std::string contentHash, long long bytes,
                                          ModelContract contract, Provenance provenance) {
    using R = Result<ModelVersion>;
    if (modelName.empty()) return R::failure("models.name", "模型必须有名称");
    if (modelName.find('@') != std::string::npos || modelName.find('/') != std::string::npos || modelName.find('\\') != std::string::npos) {
        return R::failure("models.name", "模型名称里不能有 @ 和路径分隔符");
    }
    if (number < 1) return R::failure("models.version", "版本号从 1 开始");
    if (contentHash.empty() || bytes <= 0) return R::failure("models.content", "模型文件是空的");
    if (contract.kind.empty() || contract.inputWidth <= 0 || contract.inputHeight <= 0 || contract.inputChannels <= 0) {
        return R::failure("models.contract", "模型契约不完整：需要模型的种类和输入的尺寸");
    }
    if (provenance.kind != "trained" && provenance.kind != "imported") return R::failure("models.provenance", "来源只能是 trained 或 imported");
    // T-5：训练出来的模型必须说得清是哪次训练、用的哪份数据、怎么划分的
    if (provenance.kind == "trained" &&
        (provenance.trainingJobId.empty() || provenance.datasetVersionId.empty() || provenance.split.empty())) {
        return R::failure("models.provenance", "训练出来的模型必须记录训练任务、数据集版本和划分方式");
    }
    ModelVersion v;
    v.modelName_ = std::move(modelName);
    v.number_ = number;
    v.contentHash_ = std::move(contentHash);
    v.bytes_ = bytes;
    v.contract_ = std::move(contract);
    v.provenance_ = std::move(provenance);
    v.record(std::make_shared<ModelVersionImported>(v.id()));
    return v;
}

ModelVersion ModelVersion::restore(std::string modelName, int number, std::string contentHash, long long bytes, ModelContract contract,
                                   Provenance provenance, ModelStatus status) {
    ModelVersion v;
    v.modelName_ = std::move(modelName);
    v.number_ = number;
    v.contentHash_ = std::move(contentHash);
    v.bytes_ = bytes;
    v.contract_ = std::move(contract);
    v.provenance_ = std::move(provenance);
    v.status_ = status;
    return v;
}

Status ModelVersion::publish() {
    if (status_ == ModelStatus::Published) return Status::failure("models.already_published", "模型版本 " + id() + " 已经发布");
    status_ = ModelStatus::Published;
    record(std::make_shared<ModelVersionPublished>(id()));
    return Status::success();
}

}  // namespace vision::models
