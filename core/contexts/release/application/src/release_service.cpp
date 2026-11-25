#include "vision/release/api.h"

#include <chrono>

#include "vision/common/id.h"
#include "vision/release/domain/golden_set.h"
#include "vision/release/domain/release.h"

namespace vision::release {

namespace {
ReleaseManifest toManifest(const Release& r) {
    const char* acceptance = r.acceptanceState() == AcceptanceState::Passed   ? "passed"
                             : r.acceptanceState() == AcceptanceState::Failed ? "failed"
                                                                              : "none";
    return {r.id(), r.recipeVersionId(), r.lockedModels(), r.status() == ReleaseStatus::Withdrawn, acceptance};
}

AcceptanceView toView(const std::string& releaseId, const AcceptanceRecord& a) {
    AcceptanceView v;
    v.releaseId = releaseId;
    v.goldenSetId = a.goldenSetId;
    v.goldenSetName = a.goldenSetName;
    v.checkedAtMs = a.checkedAtMs;
    v.passed = a.passed;
    v.maxMisses = a.maxMisses;
    v.maxFalseAlarmRate = a.maxFalseAlarmRate;
    v.maxErrors = a.maxErrors;
    v.total = a.total;
    v.truthOk = a.truthOk;
    v.truthNg = a.truthNg;
    v.misses = a.misses;
    v.falseAlarms = a.falseAlarms;
    v.errors = a.errors;
    v.falseAlarmRate = a.truthOk == 0 ? 0.0 : static_cast<double>(a.falseAlarms) / a.truthOk;
    v.meanMs = a.meanMs;
    v.maxMs = a.maxMs;
    for (const auto& w : a.wrongCases) {
        const char* kind = w.observed == "ERROR" ? "error" : w.truthNg ? "miss" : "false_alarm";
        v.wrongCases.push_back({w.caseId, w.imageRef, w.truthNg, w.observed, kind});
    }
    return v;
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

Result<AcceptanceView> ReleaseService::checkAcceptance(const std::string& releaseId, const std::string& goldenSetId) {
    using R = Result<AcceptanceView>;
    if (goldenSets_ == nullptr || caseRunner_ == nullptr) {
        return R::failure("release.no_acceptance", "这个程序不能做验收检查：基准图集在方案构建平台上");
    }
    // 先把要用的东西都取出来。检测很慢，不能拿着锁做
    auto content = getContent(releaseId);
    if (!content) return content.error();
    if (content.value().manifest.withdrawn) return R::failure("release.withdrawn", "上线包已经被撤回，不再做验收检查");
    std::optional<GoldenSet> set;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        set = goldenSets_->find(goldenSetId);
    }
    if (!set) return R::failure("golden.not_found", "找不到基准图集 " + goldenSetId);

    std::vector<CaseResult> results;
    std::vector<WrongCase> wrong;
    for (const auto& c : set->cases()) {
        auto run = caseRunner_->run(content.value().recipe, c.imageRef);
        if (!run) {
            return R::failure(run.error().code, "验收检查做不下去，用例 " + c.id + "（" + c.imageRef + "）：" + run.error().message);
        }
        const std::string& verdict = run.value().verdict;
        const Observed observed = verdict == "OK" ? Observed::Ok : verdict == "NG" ? Observed::Ng : Observed::Error;
        results.push_back({c.id, observed, run.value().durationMs});
        const bool truthNg = c.truth == Truth::Ng;
        if (observed == Observed::Error || (observed == Observed::Ng) != truthNg) {
            wrong.push_back({c.id, c.imageRef, truthNg, observed == Observed::Ok ? "OK" : observed == Observed::Ng ? "NG" : "ERROR"});
        }
    }
    auto evaluation = evaluateCases(*set, results);  // 和方案助手、对比评估用的是同一份规则
    if (!evaluation) return evaluation.error();

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<EventPtr> events;
    AcceptanceView view;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto release = releases_.find(releaseId);  // 重新取：检测的这段时间里它可能被撤回了
        if (!release) return R::failure("release.not_found", "找不到上线包 " + releaseId);
        if (auto st = release->recordAcceptance(*set, evaluation.value(), std::move(wrong), now); !st) return st.error();
        if (auto st = releases_.save(*release); !st) return st.error();
        events = release->pullEvents();
        view = toView(releaseId, *release->acceptance());
    }
    events_.publishAll(events);
    return view;
}

Result<BundledRelease> ReleaseService::bundle(const std::string& releaseId) const {
    using R = Result<BundledRelease>;
    std::lock_guard<std::mutex> lock(mutex_);
    auto release = releases_.find(releaseId);
    if (!release) return R::failure("release.not_found", "找不到上线包 " + releaseId);
    if (release->status() == ReleaseStatus::Withdrawn) return R::failure("release.withdrawn", "上线包已经被撤回，不能导出");
    if (release->acceptanceState() != AcceptanceState::Passed) {
        return R::failure("release.not_accepted", release->acceptance() ? "上线包的验收检查没有通过，不能导出"
                                                                        : "上线包还没有做过验收检查，不能导出。请先验收");
    }
    return BundledRelease{release->id(), release->recipeVersionId(), release->lockedModels(), toView(releaseId, *release->acceptance())};
}

Result<bool> ReleaseService::adopt(const BundledRelease& b) {
    using R = Result<bool>;
    const AcceptanceView& v = b.acceptance;
    AcceptanceRecord a;
    a.goldenSetId = v.goldenSetId;
    a.goldenSetName = v.goldenSetName;
    a.checkedAtMs = v.checkedAtMs;
    a.passed = v.passed;
    a.maxMisses = v.maxMisses;
    a.maxFalseAlarmRate = v.maxFalseAlarmRate;
    a.maxErrors = v.maxErrors;
    a.total = v.total;
    a.truthOk = v.truthOk;
    a.truthNg = v.truthNg;
    a.misses = v.misses;
    a.falseAlarms = v.falseAlarms;
    a.errors = v.errors;
    a.meanMs = v.meanMs;
    a.maxMs = v.maxMs;
    for (const auto& w : v.wrongCases) a.wrongCases.push_back({w.caseId, w.imageRef, w.truthNg, w.observed});
    auto adopted = Release::adopt(b.releaseId, b.recipeVersionId, b.lockedModels, std::move(a));  // B1
    if (!adopted) return adopted.error();
    // 它引用的方案版本本机必须已经有了（上线包文件导入时先导入方案版本，再走到这里）
    if (auto doc = recipes_.find(b.recipeVersionId); !doc) return doc.error();

    std::vector<EventPtr> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto existing = releases_.find(b.releaseId)) {
            if (auto st = existing->sameAs(b.recipeVersionId); !st) return st.error();  // B2
            return false;
        }
        if (auto st = releases_.save(adopted.value()); !st) return st.error();
        events = adopted.value().pullEvents();
    }
    events_.publishAll(events);
    return true;
}

Result<AcceptanceView> ReleaseService::getAcceptance(const std::string& releaseId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto release = releases_.find(releaseId);
    if (!release) return Result<AcceptanceView>::failure("release.not_found", "找不到上线包 " + releaseId);
    if (!release->acceptance()) return Result<AcceptanceView>::failure("release.not_checked", "这个上线包还没有做过验收检查");
    return toView(releaseId, *release->acceptance());
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
