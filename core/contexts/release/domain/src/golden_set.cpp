#include "vision/release/domain/golden_set.h"

#include <algorithm>
#include <set>

namespace vision::release {

Result<GoldenSet> GoldenSet::create(GoldenSetId id, std::string name) {
    if (id.empty()) return Result<GoldenSet>::failure("golden.empty_id", "基准图集 ID 不能为空");
    if (name.empty()) return Result<GoldenSet>::failure("golden.empty_name", "基准图集名称不能为空");
    GoldenSet s;
    s.id_ = std::move(id);
    s.name_ = std::move(name);
    return s;
}

GoldenSet GoldenSet::restore(GoldenSetId id, std::string name, std::vector<GoldenCase> cases, AcceptanceCriteria criteria,
                             bool frozen) {
    GoldenSet s;
    s.id_ = std::move(id);
    s.name_ = std::move(name);
    s.cases_ = std::move(cases);
    s.criteria_ = criteria;
    s.frozen_ = frozen;
    return s;
}

const GoldenCase* GoldenSet::findCase(const std::string& caseId) const {
    const auto it = std::find_if(cases_.begin(), cases_.end(), [&](const GoldenCase& c) { return c.id == caseId; });
    return it == cases_.end() ? nullptr : &*it;
}

Status GoldenSet::addCase(GoldenCase goldenCase) {
    if (frozen_) return Status::failure("golden.frozen", "基准图集已经冻结，不能修改");
    if (goldenCase.id.empty()) return Status::failure("golden.empty_case_id", "用例 ID 不能为空");
    if (goldenCase.imageRef.empty()) return Status::failure("golden.no_image", "用例 " + goldenCase.id + " 没有图像");
    if (findCase(goldenCase.id) != nullptr) return Status::failure("golden.duplicate_case", "用例 ID 重复：" + goldenCase.id);
    cases_.push_back(std::move(goldenCase));
    return Status::success();
}

Status GoldenSet::setCriteria(AcceptanceCriteria criteria) {
    if (frozen_) return Status::failure("golden.frozen", "基准图集已经冻结，不能修改");
    if (criteria.maxMisses < 0 || criteria.maxErrors < 0 || criteria.maxFalseAlarmRate < 0 || criteria.maxFalseAlarmRate > 1) {
        return Status::failure("golden.bad_criteria", "验收标准不合法");
    }
    criteria_ = criteria;
    return Status::success();
}

Status GoldenSet::freeze() {
    if (frozen_) return Status::failure("golden.frozen", "基准图集已经冻结");
    if (cases_.empty()) return Status::failure("golden.empty", "基准图集里没有用例，不能冻结");
    frozen_ = true;
    record(std::make_shared<GoldenSetFrozen>(id_, static_cast<int>(cases_.size())));
    return Status::success();
}

Result<CaseEvaluation> evaluateCases(const GoldenSet& set, const std::vector<CaseResult>& results) {
    using R = Result<CaseEvaluation>;
    CaseEvaluation e;
    std::set<std::string> seen;
    double totalMs = 0;
    for (const auto& r : results) {
        const GoldenCase* c = set.findCase(r.caseId);
        if (c == nullptr) return R::failure("golden.unknown_case", "基准图集里没有用例 " + r.caseId);
        if (!seen.insert(r.caseId).second) return R::failure("golden.duplicate_result", "用例 " + r.caseId + " 出现了两次");

        ++e.total;
        (c->truth == Truth::Ok ? e.truthOk : e.truthNg) += 1;
        bool wrong = false;
        if (r.observed == Observed::Error) {
            ++e.errors;
            wrong = true;
        } else if (c->truth == Truth::Ng && r.observed == Observed::Ok) {
            ++e.misses;
            wrong = true;
        } else if (c->truth == Truth::Ok && r.observed == Observed::Ng) {
            ++e.falseAlarms;
            wrong = true;
        }
        if (wrong) e.wrongCaseIds.push_back(r.caseId);
        totalMs += r.durationMs;
        e.maxMs = std::max(e.maxMs, r.durationMs);
    }
    if (e.total > 0) e.meanMs = totalMs / e.total;
    return e;
}

bool meets(const CaseEvaluation& e, const AcceptanceCriteria& c) {
    return e.errors <= c.maxErrors && e.misses <= c.maxMisses && e.falseAlarmRate() <= c.maxFalseAlarmRate + 1e-12;
}

bool betterThan(const CaseEvaluation& a, const CaseEvaluation& b) {
    if (a.errors != b.errors) return a.errors < b.errors;
    if (a.misses != b.misses) return a.misses < b.misses;
    return a.falseAlarms < b.falseAlarms;
}

}  // namespace vision::release
