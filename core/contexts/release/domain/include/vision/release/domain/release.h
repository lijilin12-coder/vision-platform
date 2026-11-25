#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::release {

using ReleaseId = std::string;

enum class ReleaseStatus { Active, Withdrawn };

// 验收检查里判错的一个用例
struct WrongCase {
    std::string caseId;
    std::string imageRef;
    bool truthNg = false;      // 人确认的真实判定
    std::string observed;      // 检测给出的：OK / NG / ERROR
};

// 实体：验收记录。一次验收检查留下来的东西：用的哪个基准图集、当时的验收标准、结果如何。
// 标准和数字都抄一份存在这里：以后回头看这条记录，不用再去找当时的基准图集。
struct AcceptanceRecord {
    std::string goldenSetId, goldenSetName;
    std::int64_t checkedAtMs = 0;  // 1970-01-01 UTC 以来的毫秒数
    bool passed = false;
    // 当时的验收标准
    int maxMisses = 0;
    double maxFalseAlarmRate = 0;
    int maxErrors = 0;
    // 结果
    int total = 0, truthOk = 0, truthNg = 0;
    int misses = 0, falseAlarms = 0, errors = 0;
    double meanMs = 0, maxMs = 0;
    std::vector<WrongCase> wrongCases;
};

enum class AcceptanceState { NotChecked, Passed, Failed };

// 聚合根：上线包（docs/ddd/05-tactical-design.md §4.1）。
// 上线包 = 方案版本 + 锁定的具体模型版本，是验证、部署、热切换、回滚的单位（H-08）。
//
// 这里是 POC-9 需要的最小实现：
//   L1 方案版本和锁定的模型版本创建后不可修改（只提供只读访问）
//   L6 已撤回的上线包不能再部署；撤回不可撤销
//   L4（一部分）验收检查没有通过的上线包不能部署。
//       还没有做过验收检查的可以部署——见 docs/ddd/15-acceptance-check.md §4 为什么先这样
//   A1  已撤回的上线包不再做验收检查
//   A2  验收记录必须覆盖基准图集的全部用例，并且至少有一个用例
//   A3  通过还是没通过由验收标准算出来，不由调用的一方说了算（规则在 golden_set.h 的 meets）
//   A4  可以重新验收（例如换一个更全的基准图集）；以最近的一次为准
// 跨平台一致性检查（L4 的另一半、L5）以及模型兼容性（L2）还没有实现。
class Release : public EventRecorder {
public:
    // 创建上线包。recipeVersionId 不能为空。
    static Result<Release> create(ReleaseId id, std::string recipeVersionId,
                                  std::map<std::string, std::string> lockedModels = {});

    // 从存储中恢复，不产生事件。
    static Release restore(ReleaseId id, std::string recipeVersionId, std::map<std::string, std::string> lockedModels,
                           ReleaseStatus status, std::optional<AcceptanceRecord> acceptance = std::nullopt);

    // 撤回：这个上线包有问题，不允许再部署到任何工位（F-5）。重复撤回被拒绝。
    Status withdraw();

    // 记下一次验收检查的结果。evaluation 是对 set 的全部用例的评估；wrongCases 是判错的那些，给人看。
    Status recordAcceptance(const class GoldenSet& set, const struct CaseEvaluation& evaluation, std::vector<WrongCase> wrongCases,
                            std::int64_t checkedAtMs);

    bool deployable() const { return status_ == ReleaseStatus::Active && acceptanceState() != AcceptanceState::Failed; }
    AcceptanceState acceptanceState() const {
        return !acceptance_ ? AcceptanceState::NotChecked : acceptance_->passed ? AcceptanceState::Passed : AcceptanceState::Failed;
    }
    const std::optional<AcceptanceRecord>& acceptance() const { return acceptance_; }

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
    std::optional<AcceptanceRecord> acceptance_;
};

struct ReleaseAccepted final : DomainEvent {
    std::string releaseId, goldenSetId;
    ReleaseAccepted(std::string r, std::string g) : releaseId(std::move(r)), goldenSetId(std::move(g)) {}
    const char* name() const override { return "ReleaseAccepted"; }
};

struct ReleaseAcceptanceFailed final : DomainEvent {
    std::string releaseId, goldenSetId;
    ReleaseAcceptanceFailed(std::string r, std::string g) : releaseId(std::move(r)), goldenSetId(std::move(g)) {}
    const char* name() const override { return "ReleaseAcceptanceFailed"; }
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
