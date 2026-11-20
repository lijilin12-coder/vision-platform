#include "report_markdown.h"

#include <iomanip>
#include <sstream>

namespace vision::assistant {

namespace {

std::string num(double v) {
    std::ostringstream os;
    os << std::setprecision(6) << v;
    return os.str();
}

std::string yesNo(bool b) { return b ? "达标" : "**不达标**"; }

void evaluationRow(std::ostringstream& os, const std::string& label, const Evaluation& e) {
    os << "| " << label << " | " << e.total() << " | " << e.truthOk << " / " << e.truthNg << " | " << e.misses << " | "
       << e.falseAlarms << " | " << e.errors << " | " << std::fixed << std::setprecision(2) << e.meanMs << " | "
       << yesNo(e.meetsCriteria) << " |\n";
    os.unsetf(std::ios::fixed);
}

const char* reasonText(EndReason r) {
    switch (r) {
        case EndReason::CriteriaMet: return "调参集上达到了验收标准";
        case EndReason::BudgetExhausted: return "预算用尽";
        case EndReason::AdvisorFinished: return "顾问认为不必再改";
        case EndReason::Cancelled: return "被取消";
        case EndReason::Failed: return "出错";
        case EndReason::None: break;
    }
    return "未结束";
}

std::string describeChanges(const Proposal& p) {
    if (p.finish) return "（建议结束）";
    std::string s;
    for (const auto& c : p.changes) {
        if (!s.empty()) s += "；";
        switch (c.kind) {
            case ProposedChange::Kind::SetParam: s += c.nodeKey + "." + c.param + " = " + num(c.value); break;
            case ProposedChange::Kind::SetRoi: s += "修改 " + c.nodeKey + " 的 ROI"; break;
            case ProposedChange::Kind::AddNode: s += "添加节点 " + c.nodeKey; break;
            case ProposedChange::Kind::RemoveNode: s += "删除节点 " + c.nodeKey; break;
            case ProposedChange::Kind::Connect: s += "添加连线 " + c.nodeKey; break;
            case ProposedChange::Kind::Disconnect: s += "删除连线 " + c.nodeKey; break;
        }
    }
    return s.empty() ? "（空）" : s;
}

}  // namespace

std::string renderReport(const SessionState& s, const Workflow& workflow) {
    std::ostringstream os;
    os << "# 助手报告\n\n";
    os << "- 会话：`" << s.id << "`\n";
    os << "- 目标：" << s.brief.goal << "\n";
    os << "- 工作流：" << workflow.name() << "（`" << workflow.id() << "` v" << workflow.version() << "）\n";
    os << "- 顾问：" << s.advisor << "；数据外发：" << toString(s.brief.dataSharing) << "\n";
    os << "- 候选方案：`" << s.candidateRecipeId << "`\n";
    os << "- 状态：" << toString(s.status) << "；结束原因：" << reasonText(s.endReason) << "\n";
    if (!s.report) {
        os << "\n（报告还没有生成）\n";
        return os.str();
    }
    const AssistReport& r = *s.report;
    os << "- 用了 " << r.rounds << " 轮（保留 " << r.kept << "，回退 " << r.rolledBack << "，被拒绝 " << r.rejected << "），共 "
       << std::fixed << std::setprecision(2) << r.seconds << " 秒\n\n";
    os.unsetf(std::ios::fixed);

    os << "## 结论\n\n";
    if (r.overfitSuspected) {
        os << "**不建议直接采纳。** 候选方案在调参集上" << (r.tuning.meetsCriteria ? "达标" : "不达标")
           << "，但在它没有见过的盲测集上明显变差。\n\n";
    } else if (r.tuning.meetsCriteria && (!r.blind || r.blind->meetsCriteria)) {
        os << "候选方案在调参集" << (r.blind ? "和盲测集上都" : "上") << "达到了验收标准。是否采纳由你决定。\n\n";
    } else {
        os << "**候选方案没有达到验收标准。**\n\n";
    }

    os << "## 评估\n\n";
    os << "| | 用例数 | 合格 / 不合格 | 漏检 | 误报 | 出错 | 平均耗时 ms | 验收标准 |\n|---|---|---|---|---|---|---|---|\n";
    evaluationRow(os, "初始参数（调参集）", r.baseline);
    evaluationRow(os, "候选方案（调参集）", r.tuning);
    if (r.blind) evaluationRow(os, "候选方案（盲测集）", *r.blind);
    os << "\n";
    if (!r.tuning.wrongCaseIds.empty()) {
        os << "调参集上判错的用例：";
        for (const auto& c : r.tuning.wrongCaseIds) os << "`" << c << "` ";
        os << "\n\n";
    }
    if (r.blind && !r.blind->wrongCaseIds.empty()) {
        os << "盲测集上判错的用例：";
        for (const auto& c : r.blind->wrongCaseIds) os << "`" << c << "` ";
        os << "\n\n";
    }

    os << "## 参数\n\n| 可调项 | 范围 | 初始值 | 最终值 |\n|---|---|---|---|\n";
    for (const auto& t : workflow.tunables()) {
        const auto i = r.initialValues.find(t.name());
        const auto f = r.finalValues.find(t.name());
        os << "| " << t.name() << " | " << num(t.min) << " ~ " << num(t.max) << " | "
           << (i == r.initialValues.end() ? "-" : num(i->second)) << " | "
           << (f == r.finalValues.end() ? "-" : num(f->second)) << " |\n";
    }

    if (!r.robustness.empty()) {
        os << "\n## 稳健性检查\n\n对调参集的图像做已知的扰动之后重新评估。\n\n";
        os << "| 扰动 | 程度 | 漏检 | 误报 | 出错 | 验收标准 |\n|---|---|---|---|---|---|\n";
        for (const auto& x : r.robustness) {
            os << "| " << (x.kind == "brightness" ? "亮度 ×" : x.kind == "noise" ? "噪声 σ" : x.kind) << " | " << num(x.level)
               << " | " << x.misses << " | " << x.falseAlarms << " | " << x.errors << " | " << yesNo(x.meetsCriteria) << " |\n";
        }
    }

    os << "\n## 每一轮\n\n| 轮 | 提议 | 理由 | 结果 | 漏检 / 误报 / 出错 | 耗时 s |\n|---|---|---|---|---|---|\n";
    for (const auto& round : s.rounds) {
        std::string result;
        switch (round.outcome) {
            case RoundOutcome::RejectedByRules: result = "被规则拒绝：" + round.rejectReason; break;
            case RoundOutcome::RejectedByEditor: result = "被方案编排拒绝：" + round.rejectReason; break;
            case RoundOutcome::AdvisorFinished: result = "顾问建议结束"; break;
            case RoundOutcome::Pending: result = "未完成"; break;
            case RoundOutcome::Applied:
                result = round.decision == RoundDecision::Kept ? "保留" : round.decision == RoundDecision::RolledBack ? "回退" : "未评估";
                break;
        }
        os << "| " << round.index << " | " << describeChanges(round.proposal) << " | " << round.proposal.rationale << " | " << result
           << " | ";
        if (round.evaluation) {
            os << round.evaluation->misses << " / " << round.evaluation->falseAlarms << " / " << round.evaluation->errors;
        } else {
            os << "-";
        }
        os << " | " << std::fixed << std::setprecision(3) << round.seconds << " |\n";
        os.unsetf(std::ios::fixed);
    }

    os << "\n## 提示和局限\n\n";
    if (r.notes.empty()) os << "（无）\n";
    for (const auto& n : r.notes) os << "- " << n << "\n";
    return os.str();
}

}  // namespace vision::assistant
