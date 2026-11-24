#include "vision/models/api.h"

#include "vision/models/domain/model_version.h"

namespace vision::models {

ModelService::ModelService(ModelVersionRepository& versions, ModelFiles& files, EventPublisher& events)
    : versions_(versions), files_(files), events_(events) {}

Result<std::string> ModelService::importModel(const std::string& modelName, const std::string& filePath, const ContractView& c,
                                              const ProvenanceView& p) {
    auto stored = files_.importFile(filePath);
    if (!stored) return stored.error();

    std::vector<EventPtr> events;
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 下一个版本号：这个模型现有的最大版本号 + 1
        int next = 1;
        const std::string prefix = modelName + "@v";
        for (const auto& existing : versions_.listIds()) {
            if (existing.rfind(prefix, 0) != 0) continue;
            try {
                next = std::max(next, std::stoi(existing.substr(prefix.size())) + 1);
            } catch (const std::exception&) {
            }
        }
        auto version = ModelVersion::create(modelName, next, stored.value().sha256, stored.value().bytes,
                                            {c.kind, c.inputWidth, c.inputHeight, c.inputChannels, c.output, c.numbers},
                                            {p.kind, p.trainingJobId, p.datasetVersionId, p.split, p.backend});
        if (!version) return version.error();
        if (auto st = versions_.save(version.value()); !st) return st.error();
        events = version.value().pullEvents();
        id = version.value().id();
    }
    events_.publishAll(events);
    return id;
}

Status ModelService::adoptModel(const std::string& modelVersionId, const std::vector<unsigned char>& bytes, const ContractView& c,
                                const ProvenanceView& p) {
    // ID 的形式是 “<模型名>@v<数字>”
    const auto at = modelVersionId.rfind("@v");
    int number = 0;
    try {
        if (at != std::string::npos && at > 0) number = std::stoi(modelVersionId.substr(at + 2));
    } catch (const std::exception&) {
    }
    if (number < 1 || modelVersionId != modelVersionId.substr(0, at) + "@v" + std::to_string(number)) {
        return Status::failure("models.bad_id", "不是合法的模型版本 ID：" + modelVersionId);
    }
    auto stored = files_.importBytes(bytes);
    if (!stored) return stored.error();

    std::lock_guard<std::mutex> lock(mutex_);
    if (auto existing = versions_.find(modelVersionId)) {
        if (existing->contentHash() == stored.value().sha256) return Status::success();
        return Status::failure("models.conflict", "已经有模型版本 " + modelVersionId + "，但内容不一样");
    }
    // 先按“新建”的规则检查一遍契约（和手工导入走同一套规则），再恢复成“已发布”
    const ModelContract contract{c.kind, c.inputWidth, c.inputHeight, c.inputChannels, c.output, c.numbers};
    const Provenance provenance{p.kind, p.trainingJobId, p.datasetVersionId, p.split, p.backend};
    const std::string modelName = modelVersionId.substr(0, at);
    auto checked = ModelVersion::create(modelName, number, stored.value().sha256, stored.value().bytes, contract, provenance);
    if (!checked) return checked.error();
    const auto adopted = ModelVersion::restore(modelName, number, stored.value().sha256, stored.value().bytes, contract, provenance,
                                               ModelStatus::Published);
    return versions_.save(adopted);
}

Status ModelService::publish(const std::string& modelVersionId) {
    std::vector<EventPtr> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto version = versions_.find(modelVersionId);
        if (!version) return Status::failure("models.not_found", "找不到模型版本 " + modelVersionId);
        if (auto st = version->publish(); !st) return st;
        if (auto st = versions_.save(*version); !st) return st;
        events = version->pullEvents();
    }
    events_.publishAll(events);
    return Status::success();
}

Result<ModelVersionView> ModelService::getModelVersion(const std::string& modelVersionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto v = versions_.find(modelVersionId);
    if (!v) return Result<ModelVersionView>::failure("models.not_found", "找不到模型版本 " + modelVersionId);
    ModelVersionView view;
    view.id = v->id();
    view.modelName = v->modelName();
    view.status = v->status() == ModelStatus::Published ? "published" : "unpublished";
    view.sha256 = v->contentHash();
    view.bytes = v->bytes();
    view.filePath = files_.pathOf(v->contentHash());
    const auto& c = v->contract();
    view.contract = {c.kind, c.inputWidth, c.inputHeight, c.inputChannels, c.output, c.numbers};
    const auto& p = v->provenance();
    view.provenance = {p.kind, p.trainingJobId, p.datasetVersionId, p.split, p.backend};
    return view;
}

std::vector<std::string> ModelService::listModelVersions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return versions_.listIds();
}

}  // namespace vision::models
