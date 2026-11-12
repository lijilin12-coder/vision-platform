#include "vision/authoring/domain/recipe.h"

#include <algorithm>
#include <memory>
#include <set>

#include "vision/authoring/domain/events.h"

namespace vision::authoring {

namespace {
Status reject(const std::string& code, const std::string& message) {
    return Status::failure("authoring." + code, message);
}
}  // namespace

// ---------- 值对象 ----------

Result<PortRef> PortRef::parse(const std::string& text) {
    const auto dot = text.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == text.size()) {
        return Result<PortRef>::failure("authoring.port_ref", "端口引用必须形如 n1.image：" + text);
    }
    return PortRef{text.substr(0, dot), text.substr(dot + 1)};
}

// ---------- 创建与重建 ----------

Recipe Recipe::create(RecipeId id, std::string name) {
    Recipe r(std::move(id), std::move(name));
    r.record(std::make_shared<RecipeCreated>(r.id_, r.name_));
    return r;
}

Recipe Recipe::restore(RecipeId id, std::string name, int nextNodeNumber, int nextVersionNumber,
                       std::vector<Node> nodes, std::vector<Edge> edges, std::vector<NamedRegion> regions) {
    Recipe r(std::move(id), std::move(name));
    r.nextNodeNumber_ = nextNodeNumber;
    r.nextVersionNumber_ = nextVersionNumber;
    r.nodes_ = std::move(nodes);
    r.edges_ = std::move(edges);
    r.regions_ = std::move(regions);
    return r;
}

// ---------- 节点 ----------

Result<NodeId> Recipe::addNode(const OperatorSignature& op, NodeLayout layout) {
    Node node;
    node.id = "n" + std::to_string(nextNodeNumber_++);
    node.op = op;
    node.params = op.defaultParams();
    node.layout = layout;
    nodes_.push_back(node);
    return node.id;
}

Status Recipe::removeNode(const NodeId& id) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.id == id; });
    if (it == nodes_.end()) return reject("node_not_found", "找不到节点 " + id);
    nodes_.erase(it);
    // R6：相连的连线一并删除
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(),
                                [&](const Edge& e) { return e.from.node == id || e.to.node == id; }),
                 edges_.end());
    return Status::success();
}

Status Recipe::moveNode(const NodeId& id, NodeLayout layout) {
    Node* node = findNode(id);
    if (!node) return reject("node_not_found", "找不到节点 " + id);
    node->layout = layout;
    return Status::success();
}

// ---------- 连线 ----------

Status Recipe::connect(const PortRef& from, const PortRef& to) {
    const Node* src = findNode(from.node);
    const Node* dst = findNode(to.node);
    if (!src) return reject("node_not_found", "找不到节点 " + from.node);
    if (!dst) return reject("node_not_found", "找不到节点 " + to.node);

    // R2：端口存在且类型兼容
    const PortDef* out = src->op.findOutput(from.port);
    const PortDef* in = dst->op.findInput(to.port);
    if (!out) return reject("port_not_found", "节点 " + from.node + " 没有输出端口 " + from.port);
    if (!in) return reject("port_not_found", "节点 " + to.node + " 没有输入端口 " + to.port);
    if (!isCompatible(out->type, in->type)) {
        return reject("type_mismatch", std::string("端口类型不兼容：") + toString(out->type) + " → " +
                                           toString(in->type));
    }

    // R3：每个输入端口最多一条连线
    const bool occupied =
        std::any_of(edges_.begin(), edges_.end(), [&](const Edge& e) { return e.to == to; });
    if (occupied) return reject("input_occupied", "输入端口 " + to.str() + " 已经有连线");

    // R1：不能形成环（包括自己连自己）
    if (from.node == to.node || reachable(to.node, from.node)) {
        return reject("cycle", "连线 " + from.str() + " → " + to.str() + " 会形成环");
    }

    edges_.push_back(Edge{from, to});
    return Status::success();
}

Status Recipe::disconnect(const PortRef& from, const PortRef& to) {
    auto it = std::find(edges_.begin(), edges_.end(), Edge{from, to});
    if (it == edges_.end()) return reject("edge_not_found", "找不到连线 " + from.str() + " → " + to.str());
    edges_.erase(it);
    return Status::success();
}

bool Recipe::reachable(const NodeId& from, const NodeId& to) const {
    std::vector<NodeId> stack{from};
    std::set<NodeId> visited;
    while (!stack.empty()) {
        NodeId cur = stack.back();
        stack.pop_back();
        if (cur == to) return true;
        if (!visited.insert(cur).second) continue;
        for (const auto& e : edges_) {
            if (e.from.node == cur) stack.push_back(e.to.node);
        }
    }
    return false;
}

// ---------- 参数与 ROI ----------

Status Recipe::setParam(const NodeId& id, const std::string& param, const ParamValue& value) {
    Node* node = findNode(id);
    if (!node) return reject("node_not_found", "找不到节点 " + id);
    const ParamDef* def = node->op.findParam(param);
    if (!def) return reject("param_not_found", "算子 " + node->op.id + " 没有参数 " + param);
    // R4
    if (auto st = def->validate(value); !st) return reject("param_invalid", st.error().message);
    node->params[param] = value;
    return Status::success();
}

Status Recipe::setRoi(const NodeId& id, RoiSpec roi) {
    Node* node = findNode(id);
    if (!node) return reject("node_not_found", "找不到节点 " + id);
    // R5：具名 ROI 必须引用存在的具名区域
    if (roi.kind() == RoiSpec::Kind::Named) {
        const bool exists = std::any_of(regions_.begin(), regions_.end(),
                                        [&](const NamedRegion& r) { return r.id == roi.region(); });
        if (!exists) return reject("region_not_found", "找不到具名区域 " + roi.region());
    }
    node->roi = std::move(roi);
    return Status::success();
}

Result<RegionId> Recipe::defineNamedRegion(std::string name, Roi roi) {
    if (name.empty()) return Result<RegionId>::failure("authoring.region_name", "具名区域必须有名称");
    const bool dup = std::any_of(regions_.begin(), regions_.end(),
                                 [&](const NamedRegion& r) { return r.name == name; });
    if (dup) return Result<RegionId>::failure("authoring.region_name", "具名区域重名：" + name);
    RegionId id = "r" + std::to_string(regions_.size() + 1);
    while (std::any_of(regions_.begin(), regions_.end(), [&](const NamedRegion& r) { return r.id == id; })) {
        id += "_";
    }
    regions_.push_back(NamedRegion{id, std::move(name), roi});
    return id;
}

Status Recipe::removeNamedRegion(const RegionId& id) {
    auto it = std::find_if(regions_.begin(), regions_.end(), [&](const NamedRegion& r) { return r.id == id; });
    if (it == regions_.end()) return reject("region_not_found", "找不到具名区域 " + id);
    // R5：被引用的具名区域不能删除
    if (regionReferenced(id)) return reject("region_in_use", "具名区域 " + it->name + " 仍被节点引用，不能删除");
    regions_.erase(it);
    return Status::success();
}

bool Recipe::regionReferenced(const RegionId& id) const {
    return std::any_of(nodes_.begin(), nodes_.end(), [&](const Node& n) {
        return n.roi.kind() == RoiSpec::Kind::Named && n.roi.region() == id;
    });
}

// ---------- 发布 ----------

Result<RecipeVersion> Recipe::publish() {
    std::vector<std::string> problems;

    // P1：所有必填输入端口都已连接
    for (const auto& n : nodes_) {
        for (const auto& in : n.op.inputs) {
            if (in.optional) continue;
            const PortRef ref{n.id, in.name};
            const bool connected =
                std::any_of(edges_.begin(), edges_.end(), [&](const Edge& e) { return e.to == ref; });
            if (!connected) problems.push_back("必填输入 " + ref.str() + " 未连接");
        }
    }
    // P5：至少有一个节点为判定提供判定依据
    const bool hasJudge = std::any_of(nodes_.begin(), nodes_.end(),
                                      [](const Node& n) { return n.op.contributesToVerdict(); });
    if (!hasJudge) problems.push_back("没有任何节点提供判定依据");

    if (!problems.empty()) {
        std::string msg = "方案不能发布：";
        for (size_t i = 0; i < problems.size(); ++i) msg += (i ? "；" : "") + problems[i];
        return Result<RecipeVersion>::failure("authoring.publish_rejected", msg);
    }

    // 快照去掉画布布局（只属于编辑器）
    std::vector<Node> snapshot = nodes_;
    for (auto& n : snapshot) n.layout = NodeLayout{};

    RecipeVersion version(RecipeVersionId{id_, nextVersionNumber_++}, name_, std::move(snapshot), edges_,
                          regions_);
    record(std::make_shared<RecipeVersionPublished>(version.id().str()));
    return version;
}

// ---------- 内部 ----------

Node* Recipe::findNode(const NodeId& id) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.id == id; });
    return it == nodes_.end() ? nullptr : &*it;
}

const Node* Recipe::findNode(const NodeId& id) const {
    auto it = std::find_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.id == id; });
    return it == nodes_.end() ? nullptr : &*it;
}

}  // namespace vision::authoring
