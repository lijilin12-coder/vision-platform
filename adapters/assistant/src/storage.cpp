#include "vision/assist_adapters/storage.h"

#include <algorithm>

#include "json_files.h"  // adapters/persistence/src：原子写入等公共部分

namespace vision::assist_adapters {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace vision::assistant;
using persistence::detail::readJson;
using persistence::detail::safeFileName;
using persistence::detail::writeFileAtomically;

namespace {

json paramToJson(const ParamValue& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
    return std::get<std::string>(v);
}

ParamValue paramFromJson(const json& j) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<double>();
    return j.get<std::string>();
}

json workflowJson(const Workflow& w) {
    json nodes = json::array(), edges = json::array(), tunables = json::array(), probes = json::array();
    for (const auto& n : w.nodes()) {
        json params = json::object();
        for (const auto& [k, v] : n.params) params[k] = paramToJson(v);
        nodes.push_back({{"key", n.key}, {"operator", n.operatorId}, {"params", params}});
    }
    for (const auto& e : w.edges()) edges.push_back(json::array({e.from, e.to}));
    for (const auto& t : w.tunables()) {
        tunables.push_back({{"node", t.nodeKey}, {"param", t.param}, {"min", t.min}, {"max", t.max},
                            {"initial", t.initial}, {"hint", t.hint}});
    }
    for (const auto& p : w.probes()) probes.push_back({{"kind", toString(p.kind)}, {"levels", p.levels}});
    return {{"schema", "vision.workflow/0.1"}, {"id", w.id()}, {"version", w.version()}, {"name", w.name()},
            {"description", w.description()}, {"nodes", nodes}, {"edges", edges}, {"tunables", tunables},
            {"robustness", probes}};
}

Result<Workflow> workflowFromJson(const json& j) {
    std::vector<WorkflowNode> nodes;
    for (const auto& jn : j.at("nodes")) {
        WorkflowNode n;
        n.key = jn.at("key").get<std::string>();
        n.operatorId = jn.at("operator").get<std::string>();
        const json params = jn.value("params", json::object());  // 先存下来：直接遍历临时对象会访问到已经销毁的内存
        for (const auto& [k, v] : params.items()) n.params[k] = paramFromJson(v);
        nodes.push_back(std::move(n));
    }
    std::vector<WorkflowEdge> edges;
    for (const auto& je : j.value("edges", json::array())) edges.push_back({je.at(0).get<std::string>(), je.at(1).get<std::string>()});
    std::vector<Tunable> tunables;
    for (const auto& jt : j.value("tunables", json::array())) {
        tunables.push_back({jt.at("node").get<std::string>(), jt.at("param").get<std::string>(), jt.at("min").get<double>(),
                            jt.at("max").get<double>(), jt.at("initial").get<double>(), jt.value("hint", "")});
    }
    std::vector<RobustnessProbe> probes;
    for (const auto& jp : j.value("robustness", json::array())) {
        RobustnessProbe p;
        const std::string kind = jp.at("kind").get<std::string>();
        if (kind == "brightness") p.kind = RobustnessProbe::Kind::Brightness;
        else if (kind == "noise") p.kind = RobustnessProbe::Kind::Noise;
        else return Result<Workflow>::failure("workflow.bad_probe", "不认识的稳健性检查：" + kind);
        p.levels = jp.at("levels").get<std::vector<double>>();
        probes.push_back(std::move(p));
    }
    return Workflow::create(j.at("id").get<std::string>(), j.value("version", 1), j.at("name").get<std::string>(),
                            j.value("description", ""), std::move(nodes), std::move(edges), std::move(tunables),
                            std::move(probes));
}

std::vector<std::string> jsonStems(const fs::path& dir) {
    std::vector<std::string> ids;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        // “<会话ID>.llm.json”是和大模型的往来记录，不是会话
        if (entry.path().extension() == ".json" && entry.path().stem().extension().empty()) {
            ids.push_back(pathToUtf8(entry.path().stem()));
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// ---- 会话 ----

json evaluationJson(const Evaluation& e) {
    return {{"cases", e.caseIds}, {"truthOk", e.truthOk}, {"truthNg", e.truthNg}, {"misses", e.misses},
            {"falseAlarms", e.falseAlarms}, {"errors", e.errors}, {"meanMs", e.meanMs}, {"maxMs", e.maxMs},
            {"meetsCriteria", e.meetsCriteria}, {"wrongCases", e.wrongCaseIds}};
}

Evaluation evaluationFrom(const json& j) {
    Evaluation e;
    e.caseIds = j.at("cases").get<std::vector<std::string>>();
    e.truthOk = j.at("truthOk").get<int>();
    e.truthNg = j.at("truthNg").get<int>();
    e.misses = j.at("misses").get<int>();
    e.falseAlarms = j.at("falseAlarms").get<int>();
    e.errors = j.at("errors").get<int>();
    e.meanMs = j.at("meanMs").get<double>();
    e.maxMs = j.at("maxMs").get<double>();
    e.meetsCriteria = j.at("meetsCriteria").get<bool>();
    e.wrongCaseIds = j.at("wrongCases").get<std::vector<std::string>>();
    return e;
}

json optionalEvaluation(const std::optional<Evaluation>& e) { return e ? evaluationJson(*e) : json(nullptr); }
std::optional<Evaluation> optionalEvaluationFrom(const json& j) {
    if (j.is_null()) return std::nullopt;
    return evaluationFrom(j);
}

json robustnessJson(const std::vector<RobustnessResult>& rs) {
    json a = json::array();
    for (const auto& r : rs) {
        a.push_back({{"kind", r.kind}, {"level", r.level}, {"total", r.total}, {"misses", r.misses},
                     {"falseAlarms", r.falseAlarms}, {"errors", r.errors}, {"meetsCriteria", r.meetsCriteria}});
    }
    return a;
}

std::vector<RobustnessResult> robustnessFrom(const json& j) {
    std::vector<RobustnessResult> rs;
    for (const auto& x : j) {
        rs.push_back({x.at("kind").get<std::string>(), x.at("level").get<double>(), x.at("total").get<int>(),
                      x.at("misses").get<int>(), x.at("falseAlarms").get<int>(), x.at("errors").get<int>(),
                      x.at("meetsCriteria").get<bool>()});
    }
    return rs;
}

template <typename E>
E enumFrom(const json& j, std::initializer_list<E> all) {
    const std::string s = j.get<std::string>();
    for (E e : all) {
        if (s == toString(e)) return e;
    }
    throw std::runtime_error("不认识的取值：" + s);
}

const char* kindName(ProposedChange::Kind k) {
    switch (k) {
        case ProposedChange::Kind::SetParam: return "set-param";
        case ProposedChange::Kind::SetRoi: return "set-roi";
        case ProposedChange::Kind::AddNode: return "add-node";
        case ProposedChange::Kind::RemoveNode: return "remove-node";
        case ProposedChange::Kind::Connect: return "connect";
        case ProposedChange::Kind::Disconnect: return "disconnect";
    }
    return "?";
}

const char* outcomeName(RoundOutcome o) {
    switch (o) {
        case RoundOutcome::Pending: return "pending";
        case RoundOutcome::Applied: return "applied";
        case RoundOutcome::RejectedByRules: return "rejected-by-rules";
        case RoundOutcome::RejectedByEditor: return "rejected-by-editor";
        case RoundOutcome::AdvisorFinished: return "advisor-finished";
    }
    return "?";
}

const char* decisionName(RoundDecision d) {
    return d == RoundDecision::Kept ? "kept" : d == RoundDecision::RolledBack ? "rolled-back" : "none";
}

template <typename E, typename F>
E byName(const std::string& s, std::initializer_list<E> all, F name) {
    for (E e : all) {
        if (s == name(e)) return e;
    }
    throw std::runtime_error("不认识的取值：" + s);
}

json roundJson(const Round& r) {
    json changes = json::array();
    for (const auto& c : r.proposal.changes) {
        changes.push_back({{"kind", kindName(c.kind)}, {"node", c.nodeKey}, {"param", c.param}, {"value", c.value}});
    }
    return {{"index", r.index},
            {"proposal", {{"changes", changes}, {"rationale", r.proposal.rationale}, {"finish", r.proposal.finish}}},
            {"outcome", outcomeName(r.outcome)},
            {"rejectReason", r.rejectReason},
            {"evaluation", optionalEvaluation(r.evaluation)},
            {"decision", decisionName(r.decision)},
            {"seconds", r.seconds}};
}

Round roundFrom(const json& j) {
    Round r;
    r.index = j.at("index").get<int>();
    const json& p = j.at("proposal");
    for (const auto& c : p.at("changes")) {
        ProposedChange change;
        change.kind = byName(c.at("kind").get<std::string>(),
                             {ProposedChange::Kind::SetParam, ProposedChange::Kind::SetRoi, ProposedChange::Kind::AddNode,
                              ProposedChange::Kind::RemoveNode, ProposedChange::Kind::Connect, ProposedChange::Kind::Disconnect},
                             kindName);
        change.nodeKey = c.at("node").get<std::string>();
        change.param = c.at("param").get<std::string>();
        change.value = c.at("value").get<double>();
        r.proposal.changes.push_back(std::move(change));
    }
    r.proposal.rationale = p.at("rationale").get<std::string>();
    r.proposal.finish = p.at("finish").get<bool>();
    r.outcome = byName(j.at("outcome").get<std::string>(),
                       {RoundOutcome::Pending, RoundOutcome::Applied, RoundOutcome::RejectedByRules,
                        RoundOutcome::RejectedByEditor, RoundOutcome::AdvisorFinished},
                       outcomeName);
    r.rejectReason = j.at("rejectReason").get<std::string>();
    r.evaluation = optionalEvaluationFrom(j.at("evaluation"));
    r.decision = byName(j.at("decision").get<std::string>(),
                        {RoundDecision::None, RoundDecision::Kept, RoundDecision::RolledBack}, decisionName);
    r.seconds = j.at("seconds").get<double>();
    return r;
}

json reportJson(const AssistReport& r) {
    return {{"endReason", toString(r.endReason)},
            {"baseline", evaluationJson(r.baseline)},
            {"tuning", evaluationJson(r.tuning)},
            {"blind", optionalEvaluation(r.blind)},
            {"overfitSuspected", r.overfitSuspected},
            {"robustness", robustnessJson(r.robustness)},
            {"initialValues", r.initialValues},
            {"finalValues", r.finalValues},
            {"rounds", r.rounds}, {"kept", r.kept}, {"rolledBack", r.rolledBack}, {"rejected", r.rejected},
            {"seconds", r.seconds},
            {"advisor", r.advisor},
            {"notes", r.notes}};
}

const std::initializer_list<EndReason> kEndReasons = {EndReason::None, EndReason::CriteriaMet, EndReason::BudgetExhausted,
                                                      EndReason::AdvisorFinished, EndReason::Cancelled, EndReason::Failed};

AssistReport reportFrom(const json& j) {
    AssistReport r;
    r.endReason = enumFrom(j.at("endReason"), kEndReasons);
    r.baseline = evaluationFrom(j.at("baseline"));
    r.tuning = evaluationFrom(j.at("tuning"));
    r.blind = optionalEvaluationFrom(j.at("blind"));
    r.overfitSuspected = j.at("overfitSuspected").get<bool>();
    r.robustness = robustnessFrom(j.at("robustness"));
    r.initialValues = j.at("initialValues").get<std::map<std::string, double>>();
    r.finalValues = j.at("finalValues").get<std::map<std::string, double>>();
    r.rounds = j.at("rounds").get<int>();
    r.kept = j.at("kept").get<int>();
    r.rolledBack = j.at("rolledBack").get<int>();
    r.rejected = j.at("rejected").get<int>();
    r.seconds = j.at("seconds").get<double>();
    r.advisor = j.at("advisor").get<std::string>();
    r.notes = j.at("notes").get<std::vector<std::string>>();
    return r;
}

}  // namespace

// ---------------- 工作流 ----------------

Result<Workflow> parseWorkflow(const std::string& text) try {
    return workflowFromJson(json::parse(text));
} catch (const std::exception& e) {
    return Result<Workflow>::failure("workflow.bad_file", std::string("工作流文件格式不对：") + e.what());
}

std::string workflowToJson(const Workflow& workflow) { return workflowJson(workflow).dump(2); }

JsonWorkflowRepository::JsonWorkflowRepository(fs::path root) : dir_(std::move(root) / "workflows") {}

Result<std::string> JsonWorkflowRepository::importFile(const fs::path& file) {
    using R = Result<std::string>;
    std::ifstream in(file, std::ios::binary);
    if (!in) return R::failure("workflow.bad_file", "无法读取 " + pathToUtf8(file));
    std::stringstream ss;
    ss << in.rdbuf();
    auto workflow = parseWorkflow(ss.str());
    if (!workflow) return workflow.error();
    if (!safeFileName(workflow.value().id())) return R::failure("persistence.bad_id", "工作流 ID 不能用作文件名");
    if (auto st = writeFileAtomically(dir_ / (workflow.value().id() + ".json"), workflowToJson(workflow.value())); !st) {
        return st.error();
    }
    return workflow.value().id();
}

std::optional<Workflow> JsonWorkflowRepository::find(const std::string& id) const try {
    if (!safeFileName(id)) return std::nullopt;
    auto j = readJson(dir_ / (id + ".json"));
    if (!j) return std::nullopt;
    auto w = workflowFromJson(*j);
    if (!w) return std::nullopt;
    return std::move(w).value();
} catch (const std::exception&) {
    return std::nullopt;
}

std::vector<std::string> JsonWorkflowRepository::list() const { return jsonStems(dir_); }

// ---------------- 助手会话 ----------------

JsonAssistSessionRepository::JsonAssistSessionRepository(fs::path root) : dir_(std::move(root) / "assist-sessions") {}

fs::path JsonAssistSessionRepository::fileOf(const SessionId& id) const { return dir_ / (id + ".json"); }

Status JsonAssistSessionRepository::save(const AssistSession& session) {
    const SessionState& s = session.state();
    if (!safeFileName(s.id)) return Status::failure("persistence.bad_id", "会话 ID 不能用作文件名：" + s.id);
    json rounds = json::array();
    for (const auto& r : s.rounds) rounds.push_back(roundJson(r));
    const json j = {
        {"schema", "vision.assist-session/0.1"},
        {"id", s.id},
        {"brief", {{"goal", s.brief.goal}, {"goldenSet", s.brief.goldenSetId}, {"blindCases", s.brief.blindCaseIds},
                   {"dataSharing", toString(s.brief.dataSharing)}}},
        {"budget", {{"maxRounds", s.budget.maxRounds}, {"maxSeconds", s.budget.maxSeconds}}},
        {"advisor", s.advisor},
        {"candidateRecipe", s.candidateRecipeId},
        {"workflow", workflowJson(session.workflow())},
        {"tuningCases", s.tuningCaseIds},
        {"initialValues", s.initialValues},
        {"currentValues", s.currentValues},
        {"bestValues", s.bestValues},
        {"baseline", optionalEvaluation(s.baseline)},
        {"best", optionalEvaluation(s.best)},
        {"rounds", rounds},
        {"status", toString(s.status)},
        {"endReason", toString(s.endReason)},
        {"blind", optionalEvaluation(s.blind)},
        {"robustness", robustnessJson(s.robustness)},
        {"robustnessRecorded", s.robustnessRecorded},
        {"report", s.report ? reportJson(*s.report) : json(nullptr)}};
    return writeFileAtomically(fileOf(s.id), j.dump(2));
}

std::optional<AssistSession> JsonAssistSessionRepository::find(const SessionId& id) const try {
    if (!safeFileName(id)) return std::nullopt;
    auto read = readJson(fileOf(id));
    if (!read) return std::nullopt;
    const json& j = *read;
    auto workflow = workflowFromJson(j.at("workflow"));
    if (!workflow) return std::nullopt;

    SessionState s;
    s.id = j.at("id").get<std::string>();
    const json& b = j.at("brief");
    s.brief.goal = b.at("goal").get<std::string>();
    s.brief.goldenSetId = b.at("goldenSet").get<std::string>();
    s.brief.blindCaseIds = b.at("blindCases").get<std::vector<std::string>>();
    s.brief.dataSharing = enumFrom(b.at("dataSharing"), {DataSharing::NoImages, DataSharing::Thumbnails, DataSharing::Originals});
    s.budget.maxRounds = j.at("budget").at("maxRounds").get<int>();
    s.budget.maxSeconds = j.at("budget").at("maxSeconds").get<double>();
    s.advisor = j.at("advisor").get<std::string>();
    s.candidateRecipeId = j.at("candidateRecipe").get<std::string>();
    s.tuningCaseIds = j.at("tuningCases").get<std::vector<std::string>>();
    s.initialValues = j.at("initialValues").get<std::map<std::string, double>>();
    s.currentValues = j.at("currentValues").get<std::map<std::string, double>>();
    s.bestValues = j.at("bestValues").get<std::map<std::string, double>>();
    s.baseline = optionalEvaluationFrom(j.at("baseline"));
    s.best = optionalEvaluationFrom(j.at("best"));
    for (const auto& r : j.at("rounds")) s.rounds.push_back(roundFrom(r));
    s.status = enumFrom(j.at("status"), {SessionStatus::InProgress, SessionStatus::Ended, SessionStatus::Adopted,
                                         SessionStatus::Abandoned});
    s.endReason = enumFrom(j.at("endReason"), kEndReasons);
    s.blind = optionalEvaluationFrom(j.at("blind"));
    s.robustness = robustnessFrom(j.at("robustness"));
    s.robustnessRecorded = j.at("robustnessRecorded").get<bool>();
    if (!j.at("report").is_null()) s.report = reportFrom(j.at("report"));
    return AssistSession::restore(std::move(s), std::move(workflow).value());
} catch (const std::exception&) {
    return std::nullopt;  // POC：文件损坏视为不存在
}

std::vector<SessionId> JsonAssistSessionRepository::list() const { return jsonStems(dir_); }

}  // namespace vision::assist_adapters
