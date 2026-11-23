#pragma once

#include <string>
#include <vector>

#include "vision/authoring/domain/model.h"
#include "vision/authoring/domain/recipe_version.h"
#include "vision/common/domain_event.h"
#include "vision/common/result.h"
#include "vision/kernel/signature.h"

namespace vision::authoring {

// 聚合根：方案（含草稿）。所有编辑操作都必须通过它（docs/ddd/05-tactical-design.md §2.2）。
//
// 随时成立的不变量：
//   R1 图中没有环
//   R2 连线两端的端口存在，且端口类型兼容
//   R3 每个输入端口最多一条连线
//   R4 参数值满足算子签名中的参数定义
//   R5 具名 ROI 引用的具名区域存在；被引用的具名区域不能删除
//   R6 删除节点时，相连的连线一并删除
//   R7 模板参数的值必须是本方案里存在的匹配模板（或者为空）；仍被节点引用的匹配模板不能删除
//   R8 匹配模板的名字在方案内唯一；尺寸至少 8×8，并且完全落在样图之内
//
// 发布时才检查的规则（草稿允许暂时不完整）：
//   P1 所有必填输入端口都已连接
//   P5 至少有一个节点为判定提供判定依据
//   P6 每个模板参数都已设置
class Recipe : public EventRecorder {
public:
    static Recipe create(RecipeId id, std::string name);

    // 仓储用：从存储中重建聚合，不产生事件。
    static Recipe restore(RecipeId id, std::string name, int nextNodeNumber, int nextVersionNumber,
                          std::vector<Node> nodes, std::vector<Edge> edges, std::vector<NamedRegion> regions,
                          std::vector<MatchTemplate> templates = {});

    // ---- 命令 ----
    Result<NodeId> addNode(const OperatorSignature& op, NodeLayout layout = {});
    Status removeNode(const NodeId& id);
    Status moveNode(const NodeId& id, NodeLayout layout);
    Status connect(const PortRef& from, const PortRef& to);
    Status disconnect(const PortRef& from, const PortRef& to);
    Status setParam(const NodeId& id, const std::string& param, const ParamValue& value);
    Status setRoi(const NodeId& id, RoiSpec roi);
    Result<RegionId> defineNamedRegion(std::string name, Roi roi);
    Status removeNamedRegion(const RegionId& id);

    // 匹配模板。checkNewMatchTemplate 只检查、不修改：应用层先用它确认可以添加，再去保存图像内容，
    // 免得把一幅注定被拒绝的图像写进内容仓库。
    Status checkNewMatchTemplate(const TemplateSpec& spec) const;
    Result<TemplateId> addMatchTemplate(TemplateSpec spec);
    Status renameMatchTemplate(const TemplateId& id, std::string name);
    Status removeMatchTemplate(const TemplateId& id);

    // 发布：检查发布规则，生成新的方案版本聚合，发布 RecipeVersionPublished。
    Result<RecipeVersion> publish();

    // ---- 只读访问 ----
    const RecipeId& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }
    const std::vector<NamedRegion>& regions() const { return regions_; }
    const std::vector<MatchTemplate>& templates() const { return templates_; }
    int nextNodeNumber() const { return nextNodeNumber_; }
    int nextVersionNumber() const { return nextVersionNumber_; }

private:
    Recipe(RecipeId id, std::string name) : id_(std::move(id)), name_(std::move(name)) {}

    Node* findNode(const NodeId& id);
    const Node* findNode(const NodeId& id) const;
    bool reachable(const NodeId& from, const NodeId& to) const;  // 沿连线方向
    bool regionReferenced(const RegionId& id) const;
    bool templateReferenced(const TemplateId& id) const;
    const MatchTemplate* findTemplate(const TemplateId& id) const;

    RecipeId id_;
    std::string name_;
    int nextNodeNumber_ = 1;
    int nextVersionNumber_ = 1;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<NamedRegion> regions_;
    std::vector<MatchTemplate> templates_;
};

}  // namespace vision::authoring
