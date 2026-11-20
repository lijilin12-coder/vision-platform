#include "vision/assist_adapters/scripted_advisor.h"

#include <nlohmann/json.hpp>

namespace vision::assist_adapters {

using assistant::Proposal;
using assistant::ProposedChange;
using nlohmann::json;

ScriptedAdvisor::ScriptedAdvisor(std::string name, std::vector<Proposal> proposals)
    : name_(std::move(name)), proposals_(std::move(proposals)) {}

Result<ScriptedAdvisor> ScriptedAdvisor::fromJson(const std::string& text) try {
    using R = Result<ScriptedAdvisor>;
    const json j = json::parse(text);
    std::vector<Proposal> proposals;
    for (const auto& jp : j.at("proposals")) {
        Proposal p;
        p.rationale = jp.value("rationale", "");
        p.finish = jp.value("finish", false);
        for (const auto& jc : jp.value("changes", json::array())) {
            ProposedChange c;
            const std::string kind = jc.value("kind", "set-param");
            if (kind == "set-param") c.kind = ProposedChange::Kind::SetParam;
            else if (kind == "set-roi") c.kind = ProposedChange::Kind::SetRoi;
            else if (kind == "add-node") c.kind = ProposedChange::Kind::AddNode;
            else if (kind == "remove-node") c.kind = ProposedChange::Kind::RemoveNode;
            else if (kind == "connect") c.kind = ProposedChange::Kind::Connect;
            else if (kind == "disconnect") c.kind = ProposedChange::Kind::Disconnect;
            else return R::failure("assistant.bad_script", "脚本里有不认识的修改类型：" + kind);
            c.nodeKey = jc.value("node", "");
            c.param = jc.value("param", "");
            c.value = jc.value("value", 0.0);
            p.changes.push_back(std::move(c));
        }
        proposals.push_back(std::move(p));
    }
    return ScriptedAdvisor("scripted:" + j.value("name", std::string("unnamed")), std::move(proposals));
} catch (const std::exception& e) {
    return Result<ScriptedAdvisor>::failure("assistant.bad_script", std::string("顾问脚本格式不对：") + e.what());
}

Result<Proposal> ScriptedAdvisor::propose(const assistant::AdvisorView& view) {
    seen_.push_back(view);
    if (next_ >= proposals_.size()) {
        Proposal done;
        done.finish = true;
        done.rationale = "脚本里的提议已经用完";
        return done;
    }
    return proposals_[next_++];
}

}  // namespace vision::assist_adapters
