#include "vision/common/id.h"
#include "vision/release/api.h"
#include "vision/release/domain/golden_set.h"

namespace vision::release {

namespace {

GoldenSetView toView(const GoldenSet& s) {
    GoldenSetView v;
    v.id = s.id();
    v.name = s.name();
    for (const auto& c : s.cases()) v.cases.push_back({c.id, c.imageRef, c.truth == Truth::Ng, c.sampleId});
    v.criteria = {s.criteria().maxMisses, s.criteria().maxFalseAlarmRate, s.criteria().maxErrors};
    return v;
}

Observed toObserved(const std::string& verdict) {
    if (verdict == "OK") return Observed::Ok;
    if (verdict == "NG") return Observed::Ng;
    return Observed::Error;
}

CaseEvaluation toDomain(const EvaluationView& v) {
    CaseEvaluation e;
    e.errors = v.errors;
    e.misses = v.misses;
    e.falseAlarms = v.falseAlarms;
    return e;
}

}  // namespace

GoldenSetService::GoldenSetService(GoldenSetRepository& sets, EventPublisher& events) : sets_(sets), events_(events) {}

Result<std::string> GoldenSetService::createGoldenSet(const GoldenSetDefinition& definition) {
    auto set = GoldenSet::create(newUuid(), definition.name);
    if (!set) return set.error();
    std::vector<std::string> sampleIds;
    for (const auto& c : definition.cases) {
        std::string sampleId;
        if (sampleRegistry_ != nullptr) {
            auto sample = sampleRegistry_->sampleOf(c.imageRef, c.truthNg);
            if (!sample) return Result<std::string>::failure(sample.error().code, "用例 " + c.id + "：" + sample.error().message);
            sampleId = sample.value();
            sampleIds.push_back(sampleId);
        }
        if (auto st = set.value().addCase({c.id, c.imageRef, c.truthNg ? Truth::Ng : Truth::Ok, sampleId}); !st) return st.error();
    }
    const auto& k = definition.criteria;
    if (auto st = set.value().setCriteria({k.maxMisses, k.maxFalseAlarmRate, k.maxErrors}); !st) return st.error();
    if (auto st = set.value().freeze(); !st) return st.error();
    // 规则 N1：冻结基准图集时，其中的样本的用途固定为“留作基准”；有已经用于训练的，拒绝冻结。
    // 放在保存之前：被拒绝的图集不会留下来。
    if (sampleRegistry_ != nullptr) {
        if (auto st = sampleRegistry_->reserveForBenchmark(sampleIds); !st) return st.error();
    }

    std::vector<EventPtr> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto st = sets_.save(set.value()); !st) return st.error();
        events = set.value().pullEvents();
    }
    events_.publishAll(events);
    return set.value().id();
}

Result<GoldenSetView> GoldenSetService::getGoldenSet(const std::string& goldenSetId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto set = sets_.find(goldenSetId);
    if (!set) return Result<GoldenSetView>::failure("golden.not_found", "找不到基准图集 " + goldenSetId);
    return toView(*set);
}

std::vector<std::string> GoldenSetService::listGoldenSets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sets_.listIds();
}

Result<EvaluationView> GoldenSetService::evaluate(const std::string& goldenSetId,
                                                  const std::vector<CaseResultView>& results) const {
    using R = Result<EvaluationView>;
    std::optional<GoldenSet> set;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        set = sets_.find(goldenSetId);
    }
    if (!set) return R::failure("golden.not_found", "找不到基准图集 " + goldenSetId);

    std::vector<CaseResult> domainResults;
    domainResults.reserve(results.size());
    for (const auto& r : results) domainResults.push_back({r.caseId, toObserved(r.verdict), r.durationMs});
    auto e = evaluateCases(*set, domainResults);
    if (!e) return e.error();

    const auto& d = e.value();
    EvaluationView v;
    v.total = d.total;
    v.truthOk = d.truthOk;
    v.truthNg = d.truthNg;
    v.misses = d.misses;
    v.falseAlarms = d.falseAlarms;
    v.errors = d.errors;
    v.falseAlarmRate = d.falseAlarmRate();
    v.meanMs = d.meanMs;
    v.maxMs = d.maxMs;
    v.meetsCriteria = meets(d, set->criteria());
    v.wrongCaseIds = d.wrongCaseIds;
    return v;
}

bool GoldenSetService::betterThan(const EvaluationView& a, const EvaluationView& b) {
    return release::betterThan(toDomain(a), toDomain(b));
}

}  // namespace vision::release
