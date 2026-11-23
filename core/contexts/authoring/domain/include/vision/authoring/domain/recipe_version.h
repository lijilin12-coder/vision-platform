#pragma once

#include <string>
#include <utility>
#include <vector>

#include "vision/authoring/domain/model.h"
#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::authoring {

// 值对象：方案版本 ID，文本形式为 "<recipeId>@v<number>"。
struct RecipeVersionId {
    RecipeId recipe;
    int number = 0;

    static Result<RecipeVersionId> parse(const std::string& text);
    std::string str() const { return recipe + "@v" + std::to_string(number); }
    bool operator==(const RecipeVersionId& o) const { return recipe == o.recipe && number == o.number; }
};

enum class VersionStatus { Active, Deprecated };

// 聚合根：方案版本。发布后的不可变快照（docs/ddd/05-tactical-design.md §2.3）。
//
// 不变量：
//   - 除 status 外，内容创建后不可修改（只提供只读访问）
//   - status 只能从 Active 变为 Deprecated
class RecipeVersion : public EventRecorder {
public:
    RecipeVersion(RecipeVersionId id, std::string recipeName, std::vector<Node> nodes, std::vector<Edge> edges,
                  std::vector<NamedRegion> regions, VersionStatus status = VersionStatus::Active,
                  std::vector<MatchTemplate> templates = {});

    Status deprecate();

    const RecipeVersionId& id() const { return id_; }
    const std::string& recipeName() const { return recipeName_; }
    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }
    const std::vector<NamedRegion>& regions() const { return regions_; }
    // 匹配模板随方案版本一起冻结：和参数一样，是这个版本的一部分
    const std::vector<MatchTemplate>& templates() const { return templates_; }
    VersionStatus status() const { return status_; }

    // 用到的算子 ID 和主版本号（D5-1：部署和导入时检查本平台是否具备）。
    std::vector<std::pair<std::string, int>> operatorRefs() const;

    // 解析节点实际使用的 ROI（具名区域在这里展开）。
    std::optional<Roi> resolveRoi(const Node& node) const;

private:
    RecipeVersionId id_;
    std::string recipeName_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    std::vector<NamedRegion> regions_;
    VersionStatus status_;
    std::vector<MatchTemplate> templates_;
};

}  // namespace vision::authoring
