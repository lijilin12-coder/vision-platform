#include "vision/assistant/domain/session.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>

namespace vision::assistant {

const char* toString(DataSharing d) {
    switch (d) {
        case DataSharing::NoImages: return "no-images";
        case DataSharing::Thumbnails: return "thumbnails";
        case DataSharing::Originals: return "originals";
    }
    return "?";
}

const char* toString(EndReason r) {
    switch (r) {
        case EndReason::None: return "none";
        case EndReason::CriteriaMet: return "criteria-met";
        case EndReason::BudgetExhausted: return "budget-exhausted";
        case EndReason::AdvisorFinished: return "advisor-finished";
        case EndReason::Cancelled: return "cancelled";
        case EndReason::Failed: return "failed";
    }
    return "?";
}

const char* toString(SessionStatus s) {
    switch (s) {
        case SessionStatus::InProgress: return "in-progress";
        case SessionStatus::Ended: return "ended";
        case SessionStatus::Adopted: return "adopted";
        case SessionStatus::Abandoned: return "abandoned";
    }
    return "?";
}

namespace {

bool contains(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

std::string keyOf(const std::string& nodeKey, const std::string& param) { return nodeKey + "." + param; }

RoundSummary summarize(const Round& r) {
    RoundSummary s;
    s.index = r.index;
    s.changes = r.proposal.changes;
    switch (r.outcome) {
        case RoundOutcome::Applied: s.outcome = "applied"; break;
        case RoundOutcome::AdvisorFinished: s.outcome = "finished"; break;
        case RoundOutcome::Pending: s.outcome = "pending"; break;
        case RoundOutcome::Abandoned: s.outcome = "abandoned"; break;
        default: s.outcome = "rejected"; break;
    }
    s.rejectReason = r.rejectReason;
    s.decision = r.decision == RoundDecision::Kept ? "kept" : r.decision == RoundDecision::RolledBack ? "rolled-back" : "none";
    if (r.evaluation) {
        s.misses = r.evaluation->misses;
        s.falseAlarms = r.evaluation->falseAlarms;
        s.errors = r.evaluation->errors;
    }
    if (!r.robustness.empty()) {
        s.robustnessWrong = 0;
        s.robustnessFailed = 0;
        for (const auto& x : r.robustness) {
            s.robustnessWrong += x.wrong();
            s.robustnessFailed += x.meetsCriteria ? 0 : 1;
        }
    }
    return s;
}

}  // namespace

bool moreRobust(const std::vector<RobustnessResult>& a, const std::vector<RobustnessResult>& b) {
    const auto key = [](const std::vector<RobustnessResult>& rs) {
        int failed = 0, wrong = 0, misses = 0;
        for (const auto& r : rs) {
            failed += r.meetsCriteria ? 0 : 1;
            wrong += r.wrong();
            misses += r.misses;
        }
        return std::array<int, 3>{failed, wrong, misses};
    };
    return key(a) < key(b);
}

// ---------------- 创建与恢复 ----------------

Result<AssistSession> AssistSession::start(SessionId id, InspectionBrief brief, Budget budget, Workflow workflow,
                                           std::string advisor, const std::vector<std::string>& allCaseIds,
                                           std::string candidateRecipeId) {
    using R = Result<AssistSession>;
    if (id.empty()) return R::failure("assistant.empty_id", "会话 ID 不能为空");
    if (candidateRecipeId.empty()) return R::failure("assistant.no_candidate", "没有候选方案");
    if (budget.maxRounds < 1 || budget.maxSeconds <= 0) return R::failure("assistant.bad_budget", "预算必须大于 0");
    if (budget.hardeningRounds < 0) return R::failure("assistant.bad_budget", "加固的轮数不能是负数");
    if (brief.mode != WorkMode::TuneWithinWorkflow) {
        return R::failure("assistant.mode_unsupported", "目前只支持“工作流内调参”，还不支持自由搭建");
    }
    if (workflow.tunables().empty()) return R::failure("assistant.no_tunables", "工作流没有声明任何可调项，没有可以调的东西");

    std::set<std::string> all(allCaseIds.begin(), allCaseIds.end());
    if (all.size() != allCaseIds.size()) return R::failure("assistant.duplicate_case", "基准图集的用例 ID 有重复");
    std::set<std::string> blind;
    for (const auto& b : brief.blindCaseIds) {
        if (all.count(b) == 0) return R::failure("assistant.unknown_blind_case", "盲测集里的用例 " + b + " 不在基准图集里");
        if (!blind.insert(b).second) return R::failure("assistant.duplicate_case", "盲测集里的用例 " + b + " 重复了");
    }

    // A12：每个模板槽正好一个模板；模板只能取自调参集
    std::set<std::string> chosen;
    for (const auto& t : brief.templates) {
        const bool known = std::any_of(workflow.templateSlots().begin(), workflow.templateSlots().end(),
                                       [&](const TemplateSlot& slot) { return slot.name == t.slot; });
        if (!known) return R::failure("assistant.unknown_template_slot", "工作流里没有模板槽 " + t.slot);
        if (!chosen.insert(t.slot).second) return R::failure("assistant.duplicate_template", "模板槽 " + t.slot + " 给了两个模板");
        if (all.count(t.caseId) == 0) return R::failure("assistant.template_case", "模板 " + t.slot + " 用的用例 " + t.caseId + " 不在基准图集里");
        if (blind.count(t.caseId) != 0) {
            return R::failure("assistant.template_from_blind",
                              "模板 " + t.slot + " 用的用例 " + t.caseId + " 属于盲测集。模板只能取自调参集：否则盲测集的内容就进了方案");
        }
        if (t.width <= 0 || t.height <= 0 || t.x < 0 || t.y < 0) return R::failure("assistant.template_box", "模板 " + t.slot + " 的框不合法");
    }
    for (const auto& slot : workflow.templateSlots()) {
        if (chosen.count(slot.name) == 0) {
            return R::failure("assistant.missing_template",
                              "工作流需要模板“" + slot.name + "”" + (slot.hint.empty() ? "" : "（" + slot.hint + "）") + "，任务书里没有提供");
        }
    }

    SessionState s;
    s.id = std::move(id);
    s.brief = std::move(brief);
    s.budget = budget;
    s.advisor = std::move(advisor);
    s.candidateRecipeId = std::move(candidateRecipeId);
    for (const auto& c : allCaseIds) {
        if (blind.count(c) == 0) s.tuningCaseIds.push_back(c);
    }
    if (s.tuningCaseIds.empty()) return R::failure("assistant.no_tuning_cases", "所有用例都划进了盲测集，没有可以用来调参的用例");
    for (const auto& t : workflow.tunables()) s.initialValues[t.name()] = t.initial;
    s.currentValues = s.initialValues;
    s.bestValues = s.initialValues;

    AssistSession session(std::move(s), std::move(workflow));
    session.record(std::make_shared<AssistSessionStarted>(session.s_.id, session.workflow_.id()));
    return session;
}

AssistSession AssistSession::restore(SessionState state, Workflow workflow) {
    return AssistSession(std::move(state), std::move(workflow));
}

// ---------------- 内部工具 ----------------

bool AssistSession::isBlind(const std::string& caseId) const { return contains(s_.brief.blindCaseIds, caseId); }

const Round* AssistSession::openRound() const {
    if (!s_.rounds.empty() && s_.rounds.back().outcome == RoundOutcome::Pending) return &s_.rounds.back();
    // “已应用但还没有评估”的一轮也算进行中
    if (!s_.rounds.empty() && s_.rounds.back().outcome == RoundOutcome::Applied && !s_.rounds.back().evaluation) {
        return &s_.rounds.back();
    }
    return nullptr;
}

Round* AssistSession::open() { return const_cast<Round*>(openRound()); }

// A4：评估必须正好覆盖指定的那些用例，一个不多、一个不少。
// 这样评估只可能来自“把这些用例都实际运行了一遍”，而不是凭空给出的数字。
Status AssistSession::requireCoverage(const Evaluation& e, const std::vector<std::string>& expected, const char* what) const {
    std::set<std::string> have(e.caseIds.begin(), e.caseIds.end());
    std::set<std::string> want(expected.begin(), expected.end());
    if (have.size() != e.caseIds.size() || have != want) {
        return Status::failure("assistant.evaluation_coverage", std::string("评估必须正好覆盖") + what + "的全部用例");
    }
    if (e.truthOk + e.truthNg != e.total()) {
        return Status::failure("assistant.evaluation_inconsistent", "评估里的数字互相对不上");
    }
    return Status::success();
}

bool AssistSession::hardeningEnabled() const { return !workflow_.probes().empty() && s_.budget.hardeningRounds > 0; }

bool AssistSession::hardening() const {
    return s_.status == SessionStatus::InProgress && s_.best && s_.best->meetsCriteria;
}

bool AssistSession::needsRobustness(const Evaluation& evaluation) const {
    return evaluation.meetsCriteria && hardeningEnabled();
}

// A4 对稳健性检查同样适用：结果必须正好对应工作流声明的每一种扰动、每一个程度，并且每一项都覆盖了整个调参集。
Status AssistSession::requireRobustness(const Evaluation& e,
                                        const std::optional<std::vector<RobustnessResult>>& robustness) const {
    if (!needsRobustness(e)) {
        if (robustness) return Status::failure("assistant.robustness_unexpected", "这次评估不需要稳健性检查的结果");
        return Status::success();
    }
    if (!robustness) return Status::failure("assistant.robustness_missing", "调参集已经达标，必须同时给出稳健性检查的结果");
    std::size_t i = 0;
    for (const auto& probe : workflow_.probes()) {
        for (const double level : probe.levels) {
            if (i >= robustness->size()) return Status::failure("assistant.robustness_coverage", "稳健性检查的结果不全");
            const RobustnessResult& r = (*robustness)[i++];
            if (r.kind != toString(probe.kind) || r.level != level || r.total != static_cast<int>(s_.tuningCaseIds.size())) {
                return Status::failure("assistant.robustness_coverage", "稳健性检查的结果和工作流声明的不一致");
            }
        }
    }
    if (i != robustness->size()) return Status::failure("assistant.robustness_coverage", "稳健性检查的结果多于工作流声明的");
    return Status::success();
}

// 有了新的“最好”之后：记下第一次达标时的状态；没有可加固的余地了就结束。
void AssistSession::afterNewBest() {
    if (!s_.best->meetsCriteria) return;
    if (!s_.bestRobustness) {  // 没有加固这回事：达标就结束
        finish(EndReason::CriteriaMet);
        return;
    }
    if (s_.valuesWhenFirstMet.empty()) {
        s_.valuesWhenFirstMet = s_.bestValues;
        s_.robustnessWhenFirstMet = *s_.bestRobustness;
    }
    const bool allPass = std::all_of(s_.bestRobustness->begin(), s_.bestRobustness->end(),
                                     [](const RobustnessResult& r) { return r.meetsCriteria; });
    if (allPass) finish(EndReason::CriteriaMet);  // 声明的扰动全部通过：没有什么可以再加固的了
}

std::vector<TunableEdit> AssistSession::editsToReach(const std::map<std::string, double>& target) const {
    std::vector<TunableEdit> edits;
    for (const auto& t : workflow_.tunables()) {
        const auto it = target.find(t.name());
        if (it != target.end()) edits.push_back({t.nodeKey, t.param, it->second});
    }
    return edits;
}

void AssistSession::finish(EndReason reason) {
    s_.status = SessionStatus::Ended;
    s_.endReason = reason;
    record(std::make_shared<AssistSessionEnded>(s_.id, reason));
}

// ---------------- 迭代 ----------------

Status AssistSession::recordBaseline(Evaluation evaluation, std::optional<std::vector<RobustnessResult>> robustness) {
    if (s_.status != SessionStatus::InProgress) return Status::failure("assistant.not_in_progress", "会话已经结束");
    if (s_.baseline) return Status::failure("assistant.baseline_exists", "初始评估已经记录过了");
    if (auto st = requireCoverage(evaluation, s_.tuningCaseIds, "调参集"); !st) return st;
    if (auto st = requireRobustness(evaluation, robustness); !st) return st;
    s_.baseline = evaluation;
    s_.best = std::move(evaluation);
    s_.bestRobustness = std::move(robustness);
    afterNewBest();  // 初始参数就已经达标：没有可加固的余地则直接结束，否则直接进入加固阶段
    return Status::success();
}

Result<int> AssistSession::beginRound(double elapsedSeconds) {
    using R = Result<int>;
    if (s_.status != SessionStatus::InProgress) return R::failure("assistant.not_in_progress", "会话已经结束");
    if (!s_.baseline) return R::failure("assistant.no_baseline", "还没有记录初始评估");
    if (s_.blind) return R::failure("assistant.blind_used", "盲测集已经评估过，不能再继续迭代");  // A7
    if (openRound() != nullptr) return R::failure("assistant.round_open", "上一轮还没有结束");      // A5
    // A6：超出预算立即结束
    if (static_cast<int>(s_.rounds.size()) >= s_.budget.maxRounds || elapsedSeconds >= s_.budget.maxSeconds) {
        // 已经达标、只是在加固时预算用尽：结果仍然是“达标”
        finish(hardening() ? EndReason::CriteriaMet : EndReason::BudgetExhausted);
        return R::failure("assistant.budget_exhausted", "预算已经用尽");
    }
    const bool inHardening = hardening();
    if (inHardening) {
        // 加固有自己的预算：用完了就以“达标”结束，交出目前最稳的候选方案
        if (s_.hardeningRounds >= s_.budget.hardeningRounds) {
            finish(EndReason::CriteriaMet);
            return R::failure("assistant.hardening_done", "加固的轮数已经用完");
        }
        ++s_.hardeningRounds;
    }
    Round r;
    r.hardening = inHardening;
    r.index = static_cast<int>(s_.rounds.size()) + 1;
    s_.rounds.push_back(std::move(r));
    return s_.rounds.back().index;
}

Result<std::vector<TunableEdit>> AssistSession::submitProposal(Proposal proposal) {
    using R = Result<std::vector<TunableEdit>>;
    Round* r = open();
    if (r == nullptr || r->outcome != RoundOutcome::Pending) return R::failure("assistant.no_round", "没有正在进行的一轮");
    r->proposal = proposal;

    if (proposal.finish) {
        r->outcome = RoundOutcome::AdvisorFinished;
        // 顾问在加固阶段说“不必再改了”：方案是达标的，结束原因仍然是“达标”
        finish(hardening() ? EndReason::CriteriaMet : EndReason::AdvisorFinished);
        return std::vector<TunableEdit>{};
    }

    // A10：只能修改工作流声明的可调项，且在范围内
    if (auto st = workflow_.validate(proposal); !st) {
        r->outcome = RoundOutcome::RejectedByRules;
        r->rejectReason = st.error().message;
        record(std::make_shared<ProposalRejected>(s_.id, r->index, r->rejectReason));
        return st.error();
    }

    pendingValues_ = s_.currentValues;
    std::vector<TunableEdit> edits;
    for (const auto& c : proposal.changes) {
        pendingValues_[keyOf(c.nodeKey, c.param)] = c.value;
        edits.push_back({c.nodeKey, c.param, c.value});
    }
    r->outcome = RoundOutcome::Applied;  // 等编辑命令执行完、评估出来之后，这一轮才算结束
    return edits;
}

std::vector<TunableEdit> AssistSession::editorRejected(const std::string& reason, double roundSeconds) {
    Round* r = open();
    if (r == nullptr) return {};
    r->outcome = RoundOutcome::RejectedByEditor;  // A2：如实记录
    r->rejectReason = reason;
    r->seconds = roundSeconds;
    pendingValues_.clear();
    record(std::make_shared<ProposalRejected>(s_.id, r->index, reason));
    // 一份提议里可能有几处修改，前面几处也许已经生效了：把候选方案恢复到这一轮之前的状态
    return editsToReach(s_.currentValues);
}

Result<std::vector<TunableEdit>> AssistSession::recordEvaluation(Evaluation evaluation, Comparison tuning,
                                                                 std::optional<std::vector<RobustnessResult>> robustness,
                                                                 double roundSeconds) {
    using R = Result<std::vector<TunableEdit>>;
    Round* r = open();
    if (r == nullptr || r->outcome != RoundOutcome::Applied) return R::failure("assistant.no_round", "没有等待评估的一轮");
    if (auto st = requireCoverage(evaluation, s_.tuningCaseIds, "调参集"); !st) return st.error();  // A4
    if (auto st = requireRobustness(evaluation, robustness); !st) return st.error();

    // 保留的条件：调参集的评估更好；或者一样好，并且两边都有稳健性检查的结果、这次的更好（A11）。
    // 调参集一样好而稳健性没有更好，不保留：避免参数无意义地漂移。
    const bool keep = tuning == Comparison::Better ||
                      (tuning == Comparison::Equal && robustness && s_.bestRobustness &&
                       moreRobust(*robustness, *s_.bestRobustness));

    r->evaluation = evaluation;
    if (robustness) r->robustness = *robustness;
    r->seconds = roundSeconds;
    s_.currentValues = pendingValues_;
    pendingValues_.clear();

    std::vector<TunableEdit> rollback;
    if (keep) {
        r->decision = RoundDecision::Kept;
        s_.best = std::move(evaluation);
        s_.bestRobustness = std::move(robustness);
        s_.bestValues = s_.currentValues;
    } else {
        r->decision = RoundDecision::RolledBack;  // 没有变好：退回到目前最好的状态
        rollback = editsToReach(s_.bestValues);
        s_.currentValues = s_.bestValues;
    }
    record(std::make_shared<ExperimentCompleted>(s_.id, r->index, keep));
    if (keep) afterNewBest();
    return rollback;
}

Status AssistSession::end(EndReason reason) {
    if (s_.status != SessionStatus::InProgress) return Status::failure("assistant.not_in_progress", "会话已经结束");
    if (reason == EndReason::None || reason == EndReason::CriteriaMet) {
        return Status::failure("assistant.bad_end_reason", "这个结束原因不能由外部指定");
    }
    // 结束时如果有一轮做到一半，把它记为没有完成
    if (Round* r = open(); r != nullptr) {
        r->outcome = RoundOutcome::Abandoned;
        r->rejectReason = "会话在这一轮进行中结束";
        pendingValues_.clear();
    }
    finish(reason);
    return Status::success();
}

// ---------------- 结束之后 ----------------

Status AssistSession::recordBlindEvaluation(Evaluation evaluation) {
    if (s_.status != SessionStatus::Ended) return Status::failure("assistant.not_ended", "会话结束之后才能评估盲测集");
    if (s_.blind) return Status::failure("assistant.blind_used", "盲测集在一次会话里只能评估一次");  // A7
    if (s_.brief.blindCaseIds.empty()) return Status::failure("assistant.no_blind_cases", "这次会话没有盲测集");
    if (auto st = requireCoverage(evaluation, s_.brief.blindCaseIds, "盲测集"); !st) return st;
    s_.blind = std::move(evaluation);
    record(std::make_shared<BlindEvaluationCompleted>(s_.id));
    return Status::success();
}

Status AssistSession::recordRobustness(std::vector<RobustnessResult> results) {
    if (s_.status != SessionStatus::Ended) return Status::failure("assistant.not_ended", "会话结束之后才能做稳健性检查");
    if (s_.report) return Status::failure("assistant.report_exists", "报告已经生成，不能再修改");
    s_.robustness = std::move(results);
    s_.robustnessRecorded = true;
    return Status::success();
}

Status AssistSession::generateReport(double totalSeconds) {
    if (s_.status != SessionStatus::Ended) return Status::failure("assistant.not_ended", "会话结束之后才能生成报告");
    if (s_.report) return Status::failure("assistant.report_exists", "报告已经生成");
    if (!s_.baseline || !s_.best) return Status::failure("assistant.no_baseline", "会话没有任何评估，无法生成报告");

    AssistReport rep;
    rep.endReason = s_.endReason;
    rep.baseline = *s_.baseline;
    rep.tuning = *s_.best;
    rep.blind = s_.blind;
    // 达标的候选方案在迭代过程中已经做过稳健性检查，直接用那份结果
    rep.robustness = s_.robustnessRecorded ? s_.robustness : (s_.bestRobustness ? *s_.bestRobustness : std::vector<RobustnessResult>{});
    rep.valuesWhenFirstMet = s_.valuesWhenFirstMet;
    rep.robustnessWhenFirstMet = s_.robustnessWhenFirstMet;
    rep.hardeningRounds = s_.hardeningRounds;
    rep.initialValues = s_.initialValues;
    rep.finalValues = s_.bestValues;
    rep.rounds = static_cast<int>(s_.rounds.size());
    for (const auto& r : s_.rounds) {
        if (r.decision == RoundDecision::Kept) ++rep.kept;
        if (r.decision == RoundDecision::RolledBack) ++rep.rolledBack;
        if (r.outcome == RoundOutcome::RejectedByRules || r.outcome == RoundOutcome::RejectedByEditor) ++rep.rejected;
    }
    rep.seconds = totalSeconds;
    rep.advisor = s_.advisor;

    // ---- 提示和局限：报告是给人做决定用的，不利的情况要明说 ----
    if (!rep.tuning.meetsCriteria) rep.notes.push_back("候选方案在调参集上没有达到验收标准。");
    if (rep.blind) {
        const auto& t = rep.tuning;
        const auto& b = *rep.blind;
        const auto rate = [](int n, int d) { return d == 0 ? 0.0 : static_cast<double>(n) / d; };
        const bool worse = rate(b.misses, b.truthNg) > rate(t.misses, t.truthNg) + 1e-9 ||
                           rate(b.falseAlarms, b.truthOk) > rate(t.falseAlarms, t.truthOk) + 1e-9 ||
                           rate(b.errors, b.total()) > rate(t.errors, t.total()) + 1e-9;
        if (worse && !b.meetsCriteria) {
            rep.overfitSuspected = true;
            rep.notes.push_back(
                "盲测集上的结果明显比调参集差，并且没有达到验收标准：候选方案可能只是适应了调参集里的图像。"
                "请检查调参集和实际生产的图像（光照、位置、缺陷类型）是否一致，再决定是否采纳。");
        }
        if (b.total() < 5) rep.notes.push_back("盲测集只有 " + std::to_string(b.total()) + " 个用例，结论不可靠。");
        if (b.truthNg == 0) rep.notes.push_back("盲测集里没有不合格的用例，无法检查漏检。");
        if (b.truthOk == 0) rep.notes.push_back("盲测集里没有合格的用例，无法检查误报。");
    } else {
        rep.notes.push_back("没有盲测集：无法判断候选方案是不是只适应了调参集。");
    }
    if (rep.tuning.truthNg == 0) rep.notes.push_back("调参集里没有不合格的用例，调出来的参数不能保证检得出缺陷。");
    if (rep.tuning.truthOk == 0) rep.notes.push_back("调参集里没有合格的用例，调出来的参数不能保证不误报。");
    const int totalCases = rep.tuning.total() + (rep.blind ? rep.blind->total() : 0);
    if (totalCases < 20) rep.notes.push_back("基准图集一共只有 " + std::to_string(totalCases) + " 个用例，偏少。");
    if (rep.hardeningRounds > 0) {
        const auto failed = [](const std::vector<RobustnessResult>& rs) {
            return std::count_if(rs.begin(), rs.end(), [](const RobustnessResult& r) { return !r.meetsCriteria; });
        };
        rep.notes.push_back("达标之后又用了 " + std::to_string(rep.hardeningRounds) + " 轮加固：稳健性检查里通不过的从 " +
                            std::to_string(failed(rep.robustnessWhenFirstMet)) + " 项变为 " +
                            std::to_string(failed(rep.robustness)) + " 项（共 " + std::to_string(rep.robustness.size()) + " 项）。");
    }
    if (s_.robustnessRecorded || s_.bestRobustness) {
        for (const auto& r : rep.robustness) {
            if (!r.meetsCriteria) {
                rep.notes.push_back("稳健性检查中有不达标的情况（见报告里的表格），说明方案对这类变化比较敏感。");
                break;
            }
        }
    } else {
        rep.notes.push_back("没有做稳健性检查。");
    }
    if (s_.endReason == EndReason::BudgetExhausted) rep.notes.push_back("预算用尽时会话结束，交出的是当时最好的候选方案。");

    s_.report = std::move(rep);
    record(std::make_shared<AssistReportGenerated>(s_.id));
    return Status::success();
}

Status AssistSession::adopt() {
    // A8：采纳是人的决定。只有会话已经结束、报告已经生成，人才有做决定的依据。
    if (s_.status == SessionStatus::InProgress) return Status::failure("assistant.not_ended", "会话还在进行，不能采纳");
    if (s_.status != SessionStatus::Ended) return Status::failure("assistant.already_decided", "这次会话已经处理过了");
    if (!s_.report) return Status::failure("assistant.no_report", "还没有生成报告，不能采纳");
    s_.status = SessionStatus::Adopted;
    record(std::make_shared<CandidateAdopted>(s_.id, s_.candidateRecipeId));
    return Status::success();
}

Status AssistSession::abandon() {
    if (s_.status == SessionStatus::Adopted || s_.status == SessionStatus::Abandoned) {
        return Status::failure("assistant.already_decided", "这次会话已经处理过了");
    }
    if (s_.status == SessionStatus::InProgress) finish(EndReason::Cancelled);
    s_.status = SessionStatus::Abandoned;
    record(std::make_shared<CandidateAbandoned>(s_.id));
    return Status::success();
}

// ---------------- 交给顾问的内容 ----------------

// 一个用例的表现能不能、以什么形式交给顾问（A3、A9）。不能给则返回空。
std::optional<CaseObservation> AssistSession::forAdvisor(const CaseObservation& o) const {
    // A3：盲测集的用例不交给顾问。即使调用的人传了进来，这里也去掉。
    if (isBlind(o.caseId) || !contains(s_.tuningCaseIds, o.caseId)) return std::nullopt;
    CaseObservation copy = o;
    // A9：按数据外发策略处理图像
    switch (s_.brief.dataSharing) {
        case DataSharing::NoImages:
            copy.imageRef.clear();
            copy.thumbnailOnly = false;
            break;
        case DataSharing::Thumbnails: copy.thumbnailOnly = true; break;
        case DataSharing::Originals: copy.thumbnailOnly = false; break;
    }
    return copy;
}

AdvisorView AssistSession::advisorView(const std::vector<CaseObservation>& observations,
                                       const std::vector<ProbeObservation>& probeObservations) const {
    AdvisorView v;
    v.goal = s_.brief.goal;
    v.workflowName = workflow_.name();
    v.workflowDescription = workflow_.description();
    v.nodes = workflow_.nodes();
    for (const auto& t : workflow_.tunables()) {
        v.tunables.push_back({t, s_.currentValues.at(t.name()), s_.bestValues.at(t.name())});
    }
    v.best = s_.best;  // 只可能是调参集上的评估：盲测集的评估存在另一个字段里，从不放进这里
    for (const auto& r : s_.rounds) {
        if (r.outcome != RoundOutcome::Pending) v.history.push_back(summarize(r));
    }
    v.roundsLeft = std::max(0, s_.budget.maxRounds - static_cast<int>(s_.rounds.size()));
    v.hardening = hardening();
    if (v.hardening) v.roundsLeft = std::min(v.roundsLeft, std::max(0, s_.budget.hardeningRounds - s_.hardeningRounds));
    if (s_.bestRobustness) v.robustness = *s_.bestRobustness;
    v.dataSharing = s_.brief.dataSharing;

    for (const auto& o : observations) {
        if (auto allowed = forAdvisor(o)) v.observations.push_back(std::move(*allowed));
    }
    if (v.hardening) {
        for (const auto& p : probeObservations) {
            if (auto allowed = forAdvisor(p.observation)) {
                allowed->imageRef.clear();  // 扰动之后的图像并不存在于磁盘上，没有可以给的
                v.probeObservations.push_back({p.kind, p.level, std::move(*allowed)});
            }
        }
    }
    return v;
}

// ---------------- 盲测集的划分 ----------------

std::vector<std::string> chooseBlindCases(const std::vector<std::pair<std::string, bool>>& cases, double fraction,
                                          unsigned seed) {
    std::vector<std::string> blind;
    if (fraction <= 0) return blind;
    fraction = std::min(fraction, 0.9);

    // 自己写一个很小的伪随机数发生器：标准库的洗牌和分布在不同平台上结果不一样，
    // 而划分必须可重现（同一份任务书在任何机器上得到同一个盲测集）。
    std::uint64_t state = 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(seed) + 1);
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };

    for (const bool truthNg : {false, true}) {
        std::vector<std::string> group;
        for (const auto& [id, ng] : cases) {
            if (ng == truthNg) group.push_back(id);
        }
        std::sort(group.begin(), group.end());  // 和输入的顺序无关
        for (std::size_t i = group.size(); i > 1; --i) std::swap(group[i - 1], group[next() % i]);

        std::size_t take = static_cast<std::size_t>(static_cast<double>(group.size()) * fraction + 0.5);
        if (group.size() >= 2) take = std::max<std::size_t>(take, 1);        // 这一类至少留一个做盲测
        take = std::min(take, group.empty() ? 0 : group.size() - 1);         // 也至少留一个用来调参
        blind.insert(blind.end(), group.begin(), group.begin() + static_cast<std::ptrdiff_t>(take));
    }
    std::sort(blind.begin(), blind.end());
    return blind;
}

}  // namespace vision::assistant
