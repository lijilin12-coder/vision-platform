#include <chrono>
#include <map>

#include "report_markdown.h"
#include "vision/assistant/api.h"
#include "vision/assistant/domain/ports.h"
#include "vision/common/id.h"

namespace vision::assistant {

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

Result<DataSharing> parseSharing(const std::string& s) {
    if (s.empty() || s == "no-images") return DataSharing::NoImages;
    if (s == "thumbnails") return DataSharing::Thumbnails;
    if (s == "originals") return DataSharing::Originals;
    return Result<DataSharing>::failure("assistant.bad_data_sharing", "数据外发策略只能是 no-images、thumbnails 或 originals");
}

EvaluationData toData(const Evaluation& e) {
    EvaluationData d;
    d.truthOk = e.truthOk;
    d.truthNg = e.truthNg;
    d.misses = e.misses;
    d.falseAlarms = e.falseAlarms;
    d.errors = e.errors;
    d.meanMs = e.meanMs;
    d.maxMs = e.maxMs;
    d.meetsCriteria = e.meetsCriteria;
    d.wrongCaseIds = e.wrongCaseIds;
    return d;
}

SessionView toView(const AssistSession& session) {
    const SessionState& s = session.state();
    SessionView v;
    v.sessionId = s.id;
    v.candidateRecipeId = s.candidateRecipeId;
    v.status = toString(s.status);
    v.endReason = toString(s.endReason);
    v.advisor = s.advisor;
    v.rounds = static_cast<int>(s.rounds.size());
    for (const auto& r : s.rounds) {
        v.roundSeconds.push_back(r.seconds);
        if (r.decision == RoundDecision::Kept) ++v.kept;
        if (r.decision == RoundDecision::RolledBack) ++v.rolledBack;
        if (r.outcome == RoundOutcome::RejectedByRules || r.outcome == RoundOutcome::RejectedByEditor) ++v.rejected;
    }
    if (s.best) v.tuningMeetsCriteria = s.best->meetsCriteria;
    if (s.blind) v.blindMeetsCriteria = s.blind->meetsCriteria;
    v.finalValues = s.bestValues;
    if (s.report) {
        v.seconds = s.report->seconds;
        v.overfitSuspected = s.report->overfitSuspected;
        v.notes = s.report->notes;
    }
    v.reportMarkdown = renderReport(s, session.workflow());
    return v;
}

// 一次会话进行中用到的东西
struct Lab {
    CandidateEditor& editor;
    TrialRunner& runner;
    GoldenCases& golden;
    std::string goldenSetId;
    std::string recipeId;
    std::map<std::string, CaseInfo> cases;          // 用例 ID → 用例
    std::map<std::string, std::string> nodeIdOf;    // 工作流里节点的名字 → 候选方案里的节点 ID
    std::map<std::string, std::string> keyOfNode;   // 反过来

    Status apply(const std::vector<TunableEdit>& edits) {
        for (const auto& e : edits) {
            if (auto st = editor.setParam(recipeId, nodeIdOf.at(e.nodeKey), e.param, ParamValue(e.value)); !st) return st;
        }
        return Status::success();
    }

    // 在一组用例上实际运行候选方案，再交给上线管理的评估规则。
    Result<Evaluation> evaluate(const std::vector<std::string>& caseIds, const std::optional<Perturbation>& perturbation,
                                std::vector<CaseObservation>* observations) {
        std::vector<CaseOutcome> outcomes;
        for (const auto& id : caseIds) {
            const CaseInfo& c = cases.at(id);
            CaseObservation o;
            o.caseId = id;
            o.truthNg = c.truthNg;
            o.imageRef = c.imageRef;
            double ms = 0;
            if (auto trial = runner.run(recipeId, c.imageRef, perturbation)) {
                o.verdict = trial.value().verdict;
                o.reasons = trial.value().reasons;
                ms = trial.value().durationMs;
                for (const auto& [name, text] : trial.value().outputs) {
                    // “节点ID.端口” → “工作流里节点的名字.端口”：顾问只认识工作流里的名字
                    const auto dot = name.find('.');
                    const auto key = keyOfNode.find(name.substr(0, dot));
                    if (dot != std::string::npos && key != keyOfNode.end()) o.outputs[key->second + name.substr(dot)] = text;
                }
            } else {
                o.verdict = "ERROR";  // 跑不起来也是一种结果，如实计入评估
                o.reasons.push_back(trial.error().message);
            }
            outcomes.push_back({id, o.verdict, ms});
            if (observations != nullptr) observations->push_back(std::move(o));
        }
        auto data = golden.evaluate(goldenSetId, outcomes);
        if (!data) return data.error();
        Evaluation e;
        e.caseIds = caseIds;
        e.truthOk = data.value().truthOk;
        e.truthNg = data.value().truthNg;
        e.misses = data.value().misses;
        e.falseAlarms = data.value().falseAlarms;
        e.errors = data.value().errors;
        e.meanMs = data.value().meanMs;
        e.maxMs = data.value().maxMs;
        e.meetsCriteria = data.value().meetsCriteria;
        e.wrongCaseIds = data.value().wrongCaseIds;
        return e;
    }
};

// 按工作流搭出候选方案
Status buildCandidate(const Workflow& wf, Lab& lab) {
    auto recipe = lab.editor.createRecipe("助手候选 - " + wf.name());
    if (!recipe) return recipe.error();
    lab.recipeId = recipe.value();
    for (const auto& n : wf.nodes()) {
        auto id = lab.editor.addNode(lab.recipeId, n.operatorId);
        if (!id) return id.error();
        lab.nodeIdOf[n.key] = id.value();
        lab.keyOfNode[id.value()] = n.key;
        for (const auto& [param, value] : n.params) {
            if (auto st = lab.editor.setParam(lab.recipeId, id.value(), param, value); !st) return st;
        }
    }
    const auto endpoint = [&lab](const std::string& s) {
        const auto dot = s.find('.');
        return lab.nodeIdOf.at(s.substr(0, dot)) + s.substr(dot);
    };
    for (const auto& e : wf.edges()) {
        if (auto st = lab.editor.connect(lab.recipeId, endpoint(e.from), endpoint(e.to)); !st) return st;
    }
    for (const auto& t : wf.tunables()) {
        if (auto st = lab.editor.setParam(lab.recipeId, lab.nodeIdOf.at(t.nodeKey), t.param, ParamValue(t.initial)); !st) return st;
    }
    return Status::success();
}

}  // namespace

AssistService::AssistService(WorkflowRepository& workflows, AssistSessionRepository& sessions, RecipeAdvisor& advisor,
                             CandidateEditor& editor, TrialRunner& runner, GoldenCases& golden, EventPublisher& events)
    : workflows_(workflows), sessions_(sessions), advisor_(advisor), editor_(editor), runner_(runner), golden_(golden),
      events_(events) {}

Result<SessionView> AssistService::run(const AssistRequest& request) {
    using R = Result<SessionView>;
    const auto started = Clock::now();

    auto workflow = workflows_.find(request.workflowId);
    if (!workflow) return R::failure("assistant.workflow_not_found", "找不到工作流 " + request.workflowId);
    auto sharing = parseSharing(request.dataSharing);
    if (!sharing) return sharing.error();
    auto cases = golden_.cases(request.goldenSetId);
    if (!cases) return cases.error();

    Lab lab{editor_, runner_, golden_, request.goldenSetId, {}, {}, {}, {}};
    std::vector<std::string> allCaseIds;
    std::vector<std::pair<std::string, bool>> withTruth;
    for (const auto& c : cases.value()) {
        lab.cases[c.id] = c;
        allCaseIds.push_back(c.id);
        withTruth.emplace_back(c.id, c.truthNg);
    }

    InspectionBrief brief;
    brief.goal = request.goal;
    brief.goldenSetId = request.goldenSetId;
    brief.dataSharing = sharing.value();
    brief.blindCaseIds =
        request.blindCaseIds.empty() ? chooseBlindCases(withTruth, request.blindFraction, request.seed) : request.blindCaseIds;

    if (auto st = buildCandidate(*workflow, lab); !st) return st.error();

    auto created = AssistSession::start(newUuid(), brief, Budget{request.maxRounds, request.maxSeconds}, *workflow,
                                        advisor_.name(), allCaseIds, lab.recipeId);
    if (!created) return created.error();
    AssistSession session = std::move(created).value();
    const auto save = [&]() -> Status {
        if (auto st = sessions_.save(session); !st) return st;
        events_.publishAll(session.pullEvents());
        return Status::success();
    };
    const std::vector<std::string> tuning = session.state().tuningCaseIds;

    // ---- 初始评估 ----
    std::vector<CaseObservation> bestObservations;
    auto baseline = lab.evaluate(tuning, std::nullopt, &bestObservations);
    if (!baseline) return baseline.error();
    if (auto st = session.recordBaseline(baseline.value()); !st) return st.error();
    if (auto st = save(); !st) return st.error();

    // ---- 迭代：循环由这里驱动，顾问每一轮只被问一次（D7-2）----
    while (session.status() == SessionStatus::InProgress) {
        const auto roundStarted = Clock::now();
        if (!session.beginRound(secondsSince(started))) break;  // 预算用尽，会话已经结束（A6）

        auto proposal = advisor_.propose(session.advisorView(bestObservations));
        if (!proposal) {
            (void)session.end(EndReason::Failed);  // 顾问出错（例如网络不通）：交出目前最好的候选方案
            break;
        }
        auto edits = session.submitProposal(std::move(proposal).value());
        if (!edits) {  // 被规则拒绝（A10），已经记录在案；下一轮顾问会在历史里看到原因
            if (auto st = save(); !st) return st.error();
            continue;
        }
        if (session.status() != SessionStatus::InProgress) break;  // 顾问建议结束

        if (auto applied = lab.apply(edits.value()); !applied) {
            // 方案编排拒绝了（A2）：如实记录，并把候选方案恢复原状
            const auto restore = session.editorRejected(applied.error().message, secondsSince(roundStarted));
            if (auto st = lab.apply(restore); !st) return st.error();
            if (auto st = save(); !st) return st.error();
            continue;
        }

        std::vector<CaseObservation> observations;
        auto evaluation = lab.evaluate(tuning, std::nullopt, &observations);
        if (!evaluation) return evaluation.error();
        const bool better = golden_.betterThan(toData(evaluation.value()), toData(*session.state().best));
        auto rollback = session.recordEvaluation(std::move(evaluation).value(), better, secondsSince(roundStarted));
        if (!rollback) return rollback.error();
        if (better) {
            bestObservations = std::move(observations);
        } else if (auto st = lab.apply(rollback.value()); !st) {
            return st.error();
        }
        if (auto st = save(); !st) return st.error();
    }

    // ---- 结束之后：盲测（只一次，A7）、稳健性检查、报告 ----
    if (!session.state().brief.blindCaseIds.empty()) {
        auto blind = lab.evaluate(session.state().brief.blindCaseIds, std::nullopt, nullptr);
        if (!blind) return blind.error();
        if (auto st = session.recordBlindEvaluation(std::move(blind).value()); !st) return st.error();
    }
    if (!workflow->probes().empty()) {
        std::vector<RobustnessResult> results;
        for (const auto& probe : workflow->probes()) {
            for (const double level : probe.levels) {
                auto e = lab.evaluate(tuning, Perturbation{toString(probe.kind), level}, nullptr);
                if (!e) return e.error();
                results.push_back({toString(probe.kind), level, e.value().total(), e.value().misses, e.value().falseAlarms,
                                   e.value().errors, e.value().meetsCriteria});
            }
        }
        if (auto st = session.recordRobustness(std::move(results)); !st) return st.error();
    }
    if (auto st = session.generateReport(secondsSince(started)); !st) return st.error();
    if (auto st = save(); !st) return st.error();
    return toView(session);
}

Status AssistService::adopt(const std::string& sessionId) {
    auto session = sessions_.find(sessionId);
    if (!session) return Status::failure("assistant.session_not_found", "找不到助手会话 " + sessionId);
    if (auto st = session->adopt(); !st) return st;
    if (auto st = sessions_.save(*session); !st) return st;
    events_.publishAll(session->pullEvents());
    return Status::success();
}

Status AssistService::abandon(const std::string& sessionId) {
    auto session = sessions_.find(sessionId);
    if (!session) return Status::failure("assistant.session_not_found", "找不到助手会话 " + sessionId);
    if (auto st = session->abandon(); !st) return st;
    if (auto st = sessions_.save(*session); !st) return st;
    events_.publishAll(session->pullEvents());
    return Status::success();
}

Result<SessionView> AssistService::getSession(const std::string& sessionId) const {
    auto session = sessions_.find(sessionId);
    if (!session) return Result<SessionView>::failure("assistant.session_not_found", "找不到助手会话 " + sessionId);
    return toView(*session);
}

std::vector<std::string> AssistService::listSessions() const { return sessions_.list(); }

}  // namespace vision::assistant
