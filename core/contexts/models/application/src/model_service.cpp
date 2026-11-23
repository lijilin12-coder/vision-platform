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
