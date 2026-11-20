#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::release {

using ReleaseId = std::string;

enum class ReleaseStatus { Active, Withdrawn };

// 聚合根：上线包（docs/ddd/05-tactical-design.md §4.1）。
// 上线包 = 方案版本 + 锁定的具体模型版本，是验证、部署、热切换、回滚的单位（H-08）。
//
// 这里是 POC-9 需要的最小实现：
//   L1 方案版本和锁定的模型版本创建后不可修改（只提供只读访问）
//   L6 已撤回的上线包不能再部署；撤回不可撤销
// 验收检查、跨平台一致性检查（L4、L5）以及模型兼容性（L2）还没有实现，
// 所以目前“可部署”只等于“没有被撤回”。
class Release : public EventRecorder {
public:
    // 创建上线包。recipeVersionId 不能为空。
    static Result<Release> create(ReleaseId id, std::string recipeVersionId,
                                  std::map<std::string, std::string> lockedModels = {});

    // 从存储中恢复，不产生事件。
    static Release restore(ReleaseId id, std::string recipeVersionId, std::map<std::string, std::string> lockedModels,
                           ReleaseStatus status);

    // 撤回：这个上线包有问题，不允许再部署到任何工位（F-5）。重复撤回被拒绝。
    Status withdraw();

    bool deployable() const { return status_ == ReleaseStatus::Active; }

    const ReleaseId& id() const { return id_; }
    const std::string& recipeVersionId() const { return recipeVersionId_; }
    const std::map<std::string, std::string>& lockedModels() const { return lockedModels_; }
    ReleaseStatus status() const { return status_; }

private:
    Release(ReleaseId id, std::string recipeVersionId, std::map<std::string, std::string> lockedModels,
            ReleaseStatus status);

    ReleaseId id_;
    std::string recipeVersionId_;
    std::map<std::string, std::string> lockedModels_;  // 模型槽位 → 模型版本 ID
    ReleaseStatus status_;
};

struct ReleaseCreated final : DomainEvent {
    std::string releaseId;
    std::string recipeVersionId;
    ReleaseCreated(std::string id, std::string rv) : releaseId(std::move(id)), recipeVersionId(std::move(rv)) {}
    const char* name() const override { return "ReleaseCreated"; }
};

struct ReleaseWithdrawn final : DomainEvent {
    std::string releaseId;
    explicit ReleaseWithdrawn(std::string id) : releaseId(std::move(id)) {}
    const char* name() const override { return "ReleaseWithdrawn"; }
};

// 仓储接口定义在领域层；实现在 adapters/persistence 或测试里。
class ReleaseRepository {
public:
    virtual ~ReleaseRepository() = default;
    virtual Status save(const Release& release) = 0;
    virtual std::optional<Release> find(const ReleaseId& id) = 0;
    virtual std::vector<ReleaseId> listIds() = 0;
};

}  // namespace vision::release
