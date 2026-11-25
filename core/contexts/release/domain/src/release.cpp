#include "vision/release/domain/release.h"

#include "vision/release/domain/golden_set.h"

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
                         ReleaseStatus status, std::optional<AcceptanceRecord> acceptance) {
    Release r(std::move(id), std::move(recipeVersionId), std::move(lockedModels), status);
    r.acceptance_ = std::move(acceptance);
    return r;
}

Status Release::recordAcceptance(const GoldenSet& set, const CaseEvaluation& e, std::vector<WrongCase> wrongCases,
                                 std::int64_t checkedAtMs) {
    if (status_ == ReleaseStatus::Withdrawn) {
        return Status::failure("release.withdrawn", "上线包已经被撤回，不再做验收检查");  // A1
    }
    if (!set.frozen()) return Status::failure("release.golden_set_not_frozen", "基准图集还没有冻结，不能用来验收");
    if (set.cases().empty() || e.total != static_cast<int>(set.cases().size())) {
        // A2：只跑了一部分用例的结果不能当验收结论
        return Status::failure("release.acceptance_incomplete", "验收检查必须跑完基准图集的全部用例（" +
                                                                    std::to_string(set.cases().size()) + " 个，实际 " +
                                                                    std::to_string(e.total) + " 个）");
    }
    AcceptanceRecord r;
    r.goldenSetId = set.id();
    r.goldenSetName = set.name();
    r.checkedAtMs = checkedAtMs;
    r.passed = meets(e, set.criteria());  // A3
    r.maxMisses = set.criteria().maxMisses;
    r.maxFalseAlarmRate = set.criteria().maxFalseAlarmRate;
    r.maxErrors = set.criteria().maxErrors;
    r.total = e.total;
    r.truthOk = e.truthOk;
    r.truthNg = e.truthNg;
    r.misses = e.misses;
    r.falseAlarms = e.falseAlarms;
    r.errors = e.errors;
    r.meanMs = e.meanMs;
    r.maxMs = e.maxMs;
    r.wrongCases = std::move(wrongCases);
    const bool passed = r.passed;
    acceptance_ = std::move(r);  // A4：以最近的一次为准
    if (passed) {
        record(std::make_shared<ReleaseAccepted>(id_, set.id()));
    } else {
        record(std::make_shared<ReleaseAcceptanceFailed>(id_, set.id()));
    }
    return Status::success();
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
