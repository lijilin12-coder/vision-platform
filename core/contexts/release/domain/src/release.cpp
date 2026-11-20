#include "vision/release/domain/release.h"

namespace vision::release {

Release::Release(ReleaseId id, std::string recipeVersionId, std::map<std::string, std::string> lockedModels,
                 ReleaseStatus status)
    : id_(std::move(id)),
      recipeVersionId_(std::move(recipeVersionId)),
      lockedModels_(std::move(lockedModels)),
      status_(status) {}

Result<Release> Release::create(ReleaseId id, std::string recipeVersionId,
                                std::map<std::string, std::string> lockedModels) {
    if (id.empty()) return Result<Release>::failure("release.empty_id", "上线包 ID 不能为空");
    if (recipeVersionId.empty()) {
        return Result<Release>::failure("release.no_recipe_version", "上线包必须包含一个方案版本");
    }
    Release release(std::move(id), std::move(recipeVersionId), std::move(lockedModels), ReleaseStatus::Active);
    release.record(std::make_shared<ReleaseCreated>(release.id_, release.recipeVersionId_));
    return release;
}

Release Release::restore(ReleaseId id, std::string recipeVersionId, std::map<std::string, std::string> lockedModels,
                         ReleaseStatus status) {
    return Release(std::move(id), std::move(recipeVersionId), std::move(lockedModels), status);
}

Status Release::withdraw() {
    if (status_ == ReleaseStatus::Withdrawn) {
        return Status::failure("release.already_withdrawn", "上线包已经被撤回");
    }
    status_ = ReleaseStatus::Withdrawn;  // L6：撤回不可撤销，没有反向的操作
    record(std::make_shared<ReleaseWithdrawn>(id_));
    return Status::success();
}

}  // namespace vision::release
