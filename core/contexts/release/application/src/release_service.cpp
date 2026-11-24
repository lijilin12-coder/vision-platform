#include "vision/release/api.h"

#include "vision/common/id.h"
#include "vision/release/domain/release.h"

namespace vision::release {

namespace {
ReleaseManifest toManifest(const Release& r) {
    return {r.id(), r.recipeVersionId(), r.lockedModels(), r.status() == ReleaseStatus::Withdrawn};
}
}  // namespace

ReleaseService::ReleaseService(ReleaseRepository& releases, const RecipeVersionSource& recipes, EventPublisher& events)
    : releases_(releases), recipes_(recipes), events_(events) {}

Result<std::string> ReleaseService::createRelease(const std::string& recipeVersionId) {
    // 上线包引用的方案版本必须存在。（“必须是有效状态”的规则 L3 要等方案版本的状态进入发布语言之后再检查。）
    auto doc = recipes_.find(recipeVersionId);
    if (!doc) return doc.error();
    if (loadCheck_ != nullptr) {
        if (auto st = loadCheck_->canLoad(doc.value()); !st) {
            return Result<std::string>::failure(st.error().code, "这个方案版本现在不能做成上线包：" + st.error().message);
        }
    }

    auto release = Release::create(newUuid(), recipeVersionId);
    if (!release) return release.error();
    std::vector<EventPtr> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto st = releases_.save(release.value()); !st) return st.error();
        events = release.value().pullEvents();
    }
    events_.publishAll(events);  // 在锁外发布：事件处理器里可能再调用本服务
    return release.value().id();
}

Status ReleaseService::withdraw(const std::string& releaseId) {
    std::vector<EventPtr> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto release = releases_.find(releaseId);
        if (!release) return Status::failure("release.not_found", "找不到上线包 " + releaseId);
        if (auto st = release->withdraw(); !st) return st;
        if (auto st = releases_.save(*release); !st) return st;
        events = release->pullEvents();
    }
    events_.publishAll(events);
    return Status::success();
}

Result<ReleaseManifest> ReleaseService::getManifest(const std::string& releaseId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto release = releases_.find(releaseId);
    if (!release) return Result<ReleaseManifest>::failure("release.not_found", "找不到上线包 " + releaseId);
    return toManifest(*release);
}

Result<ReleaseContent> ReleaseService::getContent(const std::string& releaseId) const {
    auto manifest = getManifest(releaseId);
    if (!manifest) return manifest.error();
    auto doc = recipes_.find(manifest.value().recipeVersionId);
    if (!doc) return doc.error();
    return ReleaseContent{std::move(manifest).value(), std::move(doc).value()};
}

std::vector<std::string> ReleaseService::listReleases() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return releases_.listIds();
}

}  // namespace vision::release
