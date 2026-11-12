#include "vision/authoring/domain/recipe_version.h"

#include <algorithm>
#include <memory>
#include <set>

#include "vision/authoring/domain/events.h"

namespace vision::authoring {

Result<RecipeVersionId> RecipeVersionId::parse(const std::string& text) {
    const auto at = text.rfind("@v");
    if (at == std::string::npos || at == 0 || at + 2 >= text.size()) {
        return Result<RecipeVersionId>::failure("authoring.version_id", "方案版本 ID 必须形如 <方案ID>@v1：" + text);
    }
    try {
        size_t used = 0;
        const int n = std::stoi(text.substr(at + 2), &used);
        if (used != text.size() - at - 2 || n < 1) throw std::invalid_argument("bad");
        return RecipeVersionId{text.substr(0, at), n};
    } catch (const std::exception&) {
        return Result<RecipeVersionId>::failure("authoring.version_id", "方案版本号不合法：" + text);
    }
}

RecipeVersion::RecipeVersion(RecipeVersionId id, std::string recipeName, std::vector<Node> nodes,
                             std::vector<Edge> edges, std::vector<NamedRegion> regions, VersionStatus status)
    : id_(std::move(id)),
      recipeName_(std::move(recipeName)),
      nodes_(std::move(nodes)),
      edges_(std::move(edges)),
      regions_(std::move(regions)),
      status_(status) {}

Status RecipeVersion::deprecate() {
    if (status_ == VersionStatus::Deprecated) {
        return Status::failure("authoring.already_deprecated", "方案版本 " + id_.str() + " 已经停用");
    }
    status_ = VersionStatus::Deprecated;
    record(std::make_shared<RecipeVersionDeprecated>(id_.str()));
    return Status::success();
}

std::vector<std::pair<std::string, int>> RecipeVersion::operatorRefs() const {
    std::set<std::pair<std::string, int>> refs;
    for (const auto& n : nodes_) refs.emplace(n.op.id, n.op.majorVersion);
    return {refs.begin(), refs.end()};
}

std::optional<Roi> RecipeVersion::resolveRoi(const Node& node) const {
    switch (node.roi.kind()) {
        case RoiSpec::Kind::None: return std::nullopt;
        case RoiSpec::Kind::Absolute: return node.roi.roi();
        case RoiSpec::Kind::Named: {
            auto it = std::find_if(regions_.begin(), regions_.end(),
                                   [&](const NamedRegion& r) { return r.id == node.roi.region(); });
            if (it == regions_.end()) return std::nullopt;
            return it->roi;
        }
    }
    return std::nullopt;
}

}  // namespace vision::authoring
