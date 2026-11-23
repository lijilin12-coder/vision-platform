#pragma once

// 方案编排上下文内部的实体和值对象（docs/ddd/05-tactical-design.md §2.2）。

#include <optional>
#include <string>
#include <vector>

#include "vision/common/result.h"
#include "vision/kernel/geometry.h"
#include "vision/kernel/signature.h"

namespace vision::authoring {

using RecipeId = std::string;
using NodeId = std::string;
using RegionId = std::string;
using TemplateId = std::string;

// 值对象：某个节点的某个端口，文本形式为 "n1.image"。
struct PortRef {
    NodeId node;
    std::string port;

    static Result<PortRef> parse(const std::string& text);
    std::string str() const { return node + "." + port; }
    bool operator==(const PortRef& o) const { return node == o.node && port == o.port; }
};

// 值对象：连线。
struct Edge {
    PortRef from;
    PortRef to;
    bool operator==(const Edge& o) const { return from == o.from && to == o.to; }
};

// 值对象：节点在画布上的位置。只在方案编排上下文中存在，发布时去掉（04 §4）。
struct NodeLayout {
    double x = 0;
    double y = 0;
};

// 值对象：节点的 ROI 设定，三选一（POC 阶段实现“无”“绝对”“具名”）。
class RoiSpec {
public:
    enum class Kind { None, Absolute, Named };

    static RoiSpec none() { return RoiSpec(Kind::None, std::nullopt, {}); }
    static RoiSpec absolute(Roi roi) { return RoiSpec(Kind::Absolute, roi, {}); }
    static RoiSpec named(RegionId region) { return RoiSpec(Kind::Named, std::nullopt, std::move(region)); }

    Kind kind() const { return kind_; }
    const std::optional<Roi>& roi() const { return roi_; }
    const RegionId& region() const { return region_; }

private:
    RoiSpec(Kind k, std::optional<Roi> r, RegionId g) : kind_(k), roi_(std::move(r)), region_(std::move(g)) {}
    Kind kind_;
    std::optional<Roi> roi_;
    RegionId region_;
};

// 实体：节点。方案中对某个算子的一次使用。保存添加时的算子签名快照（D5-1）。
struct Node {
    NodeId id;
    OperatorSignature op;
    ParamMap params;
    RoiSpec roi = RoiSpec::none();
    NodeLayout layout;
};

// 实体：具名区域。可被多个节点按 ID 引用。
struct NamedRegion {
    RegionId id;
    std::string name;
    Roi roi;
};

// 值对象：框选模板时的输入。
struct TemplateSpec {
    std::string name;
    std::string sha256;     // 模板图像内容的哈希（内容寻址存储，ADR-20）
    long long bytes = 0;    // 内容的字节数
    int width = 0, height = 0;          // 模板的尺寸
    double referenceX = 0, referenceY = 0;  // 基准位置：模板左上角在样图上的位置
    int sourceWidth = 0, sourceHeight = 0;  // 样图的尺寸
};

// 实体：匹配模板。从样图上框选、用于定位的一小幅图像，属于一个方案（docs/ddd/09）。
// 这里只有对内容的引用（哈希），图像数据在内容仓库里：领域层不依赖 OpenCV，方案文件也不会因此变大。
// 内容不可修改：想换一幅图，就新建一个模板。
struct MatchTemplate {
    TemplateId id;
    TemplateSpec spec;
};

}  // namespace vision::authoring
