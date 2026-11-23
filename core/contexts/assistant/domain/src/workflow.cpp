#include "vision/assistant/domain/workflow.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace vision::assistant {

const char* toString(RobustnessProbe::Kind kind) {
    return kind == RobustnessProbe::Kind::Brightness ? "brightness" : "noise";
}

namespace {
std::string nodeOf(const std::string& endpoint) { return endpoint.substr(0, endpoint.find('.')); }

// 300 而不是 300.000000
std::string num(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
}
}  // namespace

Result<Workflow> Workflow::create(std::string id, int version, std::string name, std::string description,
                                  std::vector<WorkflowNode> nodes, std::vector<WorkflowEdge> edges,
                                  std::vector<Tunable> tunables, std::vector<RobustnessProbe> probes,
                                  std::vector<TemplateSlot> templateSlots) {
    using R = Result<Workflow>;
    if (id.empty()) return R::failure("workflow.empty_id", "工作流 ID 不能为空");
    if (name.empty()) return R::failure("workflow.empty_name", "工作流名称不能为空");
    if (nodes.empty()) return R::failure("workflow.no_nodes", "工作流里没有节点");

    std::set<std::string> keys;
    for (const auto& n : nodes) {
        if (n.key.empty() || n.operatorId.empty()) return R::failure("workflow.bad_node", "节点缺少名字或算子");
        if (!keys.insert(n.key).second) return R::failure("workflow.duplicate_node", "节点名字重复：" + n.key);
    }
    for (const auto& e : edges) {
        if (e.from.find('.') == std::string::npos || e.to.find('.') == std::string::npos) {
            return R::failure("workflow.bad_edge", "连线要写成“节点.端口”的形式：" + e.from + " → " + e.to);
        }
        if (keys.count(nodeOf(e.from)) == 0 || keys.count(nodeOf(e.to)) == 0) {
            return R::failure("workflow.bad_edge", "连线引用了不存在的节点：" + e.from + " → " + e.to);
        }
    }
    std::set<std::string> tunableNames;
    for (const auto& t : tunables) {
        if (keys.count(t.nodeKey) == 0) return R::failure("workflow.bad_tunable", "可调项引用了不存在的节点：" + t.name());
        if (t.param.empty()) return R::failure("workflow.bad_tunable", "可调项缺少参数名：" + t.nodeKey);
        if (!tunableNames.insert(t.name()).second) return R::failure("workflow.duplicate_tunable", "可调项重复：" + t.name());
        if (!(t.min <= t.initial && t.initial <= t.max)) {
            return R::failure("workflow.bad_tunable", "可调项 " + t.name() + " 的初始值不在范围内");
        }
    }
    for (const auto& p : probes) {
        if (p.levels.empty()) return R::failure("workflow.bad_probe", "稳健性检查没有给出扰动的大小");
    }
    std::set<std::string> slotNames, slotTargets;
    for (const auto& slot : templateSlots) {
        if (slot.name.empty() || slot.param.empty()) return R::failure("workflow.bad_template_slot", "模板槽缺少名字或参数名");
        if (!slotNames.insert(slot.name).second) return R::failure("workflow.bad_template_slot", "模板槽重名：" + slot.name);
        if (keys.count(slot.nodeKey) == 0) return R::failure("workflow.bad_template_slot", "模板槽 " + slot.name + " 指向了不存在的节点");
        if (slot.source.find('.') == std::string::npos || keys.count(nodeOf(slot.source)) == 0) {
            return R::failure("workflow.bad_template_slot", "模板槽 " + slot.name + " 的图像来源要写成“节点.端口”，并且节点必须存在");
        }
        const std::string target = slot.nodeKey + "." + slot.param;
        if (!slotTargets.insert(target).second) return R::failure("workflow.bad_template_slot", "两个模板槽指向了同一个参数：" + target);
        // 模板由人定：它不能同时被声明成可调项
        if (tunableNames.count(target) != 0) return R::failure("workflow.bad_template_slot", target + " 是模板参数，不能同时是可调项");
    }

    Workflow w;
    w.id_ = std::move(id);
    w.version_ = version;
    w.name_ = std::move(name);
    w.description_ = std::move(description);
    w.nodes_ = std::move(nodes);
    w.edges_ = std::move(edges);
    w.tunables_ = std::move(tunables);
    w.templateSlots_ = std::move(templateSlots);
    w.probes_ = std::move(probes);
    return w;
}

const Tunable* Workflow::findTunable(const std::string& nodeKey, const std::string& param) const {
    const auto it = std::find_if(tunables_.begin(), tunables_.end(),
                                 [&](const Tunable& t) { return t.nodeKey == nodeKey && t.param == param; });
    return it == tunables_.end() ? nullptr : &*it;
}

Status Workflow::validate(const Proposal& proposal) const {
    if (proposal.changes.empty()) return Status::failure("assistant.empty_proposal", "提议里没有任何修改");
    std::set<std::string> touched;
    for (const auto& c : proposal.changes) {
        if (c.kind != ProposedChange::Kind::SetParam) {
            return Status::failure("assistant.structure_change",
                                   "工作流内调参时不能改变方案的结构，只能修改声明的可调项");
        }
        const std::string name = c.nodeKey + "." + c.param;
        const Tunable* t = findTunable(c.nodeKey, c.param);
        if (t == nullptr) {
            return Status::failure("assistant.not_tunable", "“" + name + "”不是工作流声明的可调项，不能修改");
        }
        if (!std::isfinite(c.value) || c.value < t->min || c.value > t->max) {
            return Status::failure("assistant.out_of_range",
                                   "“" + name + "”的取值 " + num(c.value) + " 超出了允许的范围 " +
                                       num(t->min) + "～" + num(t->max));
        }
        if (!touched.insert(name).second) {
            return Status::failure("assistant.duplicate_change", "同一个可调项“" + name + "”在一份提议里出现了两次");
        }
    }
    return Status::success();
}

}  // namespace vision::assistant
