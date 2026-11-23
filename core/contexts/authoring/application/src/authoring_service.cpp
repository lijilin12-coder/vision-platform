#include "vision/authoring/api.h"
#include "vision/authoring/domain/repositories.h"
#include "vision/catalog/api.h"
#include "vision/common/id.h"

namespace vision::authoring {

namespace {

Status notFound(const std::string& recipeId) {
    return Status::failure("authoring.recipe_not_found", "找不到方案 " + recipeId);
}

std::string describeRoi(const RoiSpec& roi) {
    switch (roi.kind()) {
        case RoiSpec::Kind::None: return "";
        case RoiSpec::Kind::Absolute: {
            const auto& r = *roi.roi();
            return "rect(" + std::to_string(r.x()) + "," + std::to_string(r.y()) + "," + std::to_string(r.width()) +
                   "," + std::to_string(r.height()) + ")";
        }
        case RoiSpec::Kind::Named: return "named:" + roi.region();
    }
    return "";
}

std::vector<RecipeDocumentTemplate> toDocument(const std::vector<MatchTemplate>& templates) {
    std::vector<RecipeDocumentTemplate> out;
    for (const auto& t : templates) {
        out.push_back({t.id, t.spec.name, t.spec.sha256, t.spec.bytes, t.spec.width, t.spec.height, t.spec.referenceX,
                       t.spec.referenceY, t.spec.sourceWidth, t.spec.sourceHeight});
    }
    return out;
}

}  // namespace

AuthoringService::AuthoringService(RecipeRepository& recipes, RecipeVersionRepository& versions,
                                   const catalog::OperatorCatalog& catalog, EventPublisher& events)
    : recipes_(recipes), versions_(versions), catalog_(catalog), events_(events) {}

template <typename Fn>
Status AuthoringService::modify(const std::string& recipeId, Fn&& fn) {
    auto recipe = recipes_.find(recipeId);
    if (!recipe) return notFound(recipeId);
    if (auto st = fn(*recipe); !st) return st;
    if (auto st = recipes_.save(*recipe); !st) return st;
    events_.publishAll(recipe->pullEvents());
    return Status::success();
}

Result<std::string> AuthoringService::createRecipe(const std::string& name) {
    if (name.empty()) return Result<std::string>::failure("authoring.recipe_name", "方案必须有名称");
    Recipe recipe = Recipe::create(newUuid(), name);
    if (auto st = recipes_.save(recipe); !st) return st.error();
    events_.publishAll(recipe.pullEvents());
    return recipe.id();
}

Result<std::string> AuthoringService::addNode(const std::string& recipeId, const std::string& operatorId) {
    // 从算子目录取签名快照（D5-1）。这是跨上下文协作：只通过 catalog 的公开接口。
    auto meta = catalog_.find(operatorId);
    if (!meta) return Result<std::string>::failure("authoring.operator_not_found", "找不到算子 " + operatorId);
    std::string nodeId;
    auto st = modify(recipeId, [&](Recipe& r) -> Status {
        auto res = r.addNode(meta->signature);
        if (!res) return res.error();
        nodeId = res.value();
        return Status::success();
    });
    if (!st) return st.error();
    return nodeId;
}

Status AuthoringService::removeNode(const std::string& recipeId, const std::string& nodeId) {
    return modify(recipeId, [&](Recipe& r) { return r.removeNode(nodeId); });
}

Status AuthoringService::connect(const std::string& recipeId, const std::string& from, const std::string& to) {
    auto f = PortRef::parse(from);
    if (!f) return f.error();
    auto t = PortRef::parse(to);
    if (!t) return t.error();
    return modify(recipeId, [&](Recipe& r) { return r.connect(f.value(), t.value()); });
}

Status AuthoringService::disconnect(const std::string& recipeId, const std::string& from, const std::string& to) {
    auto f = PortRef::parse(from);
    if (!f) return f.error();
    auto t = PortRef::parse(to);
    if (!t) return t.error();
    return modify(recipeId, [&](Recipe& r) { return r.disconnect(f.value(), t.value()); });
}

Status AuthoringService::setParam(const std::string& recipeId, const std::string& nodeId, const std::string& param,
                                  const ParamValue& value) {
    return modify(recipeId, [&](Recipe& r) { return r.setParam(nodeId, param, value); });
}

Status AuthoringService::setRoi(const std::string& recipeId, const std::string& nodeId, const Roi& roi) {
    return modify(recipeId, [&](Recipe& r) { return r.setRoi(nodeId, RoiSpec::absolute(roi)); });
}

Status AuthoringService::clearRoi(const std::string& recipeId, const std::string& nodeId) {
    return modify(recipeId, [&](Recipe& r) { return r.setRoi(nodeId, RoiSpec::none()); });
}

Result<std::string> AuthoringService::defineNamedRegion(const std::string& recipeId, const std::string& name,
                                                        const Roi& roi) {
    std::string regionId;
    auto st = modify(recipeId, [&](Recipe& r) -> Status {
        auto res = r.defineNamedRegion(name, roi);
        if (!res) return res.error();
        regionId = res.value();
        return Status::success();
    });
    if (!st) return st.error();
    return regionId;
}

Status AuthoringService::useNamedRegion(const std::string& recipeId, const std::string& nodeId,
                                        const std::string& regionId) {
    return modify(recipeId, [&](Recipe& r) { return r.setRoi(nodeId, RoiSpec::named(regionId)); });
}

Status AuthoringService::removeNamedRegion(const std::string& recipeId, const std::string& regionId) {
    return modify(recipeId, [&](Recipe& r) { return r.removeNamedRegion(regionId); });
}

Result<std::string> AuthoringService::createMatchTemplate(const std::string& recipeId, const std::string& name,
                                                          const Image& source, int x, int y, int width, int height) {
    using R = Result<std::string>;
    if (templateStore_ == nullptr) return R::failure("authoring.no_template_store", "这个程序没有配置保存模板的地方");
    if (source.empty()) return R::failure("authoring.template_size", "样图是空的");
    auto recipe = recipes_.find(recipeId);
    if (!recipe) return notFound(recipeId).error();

    TemplateSpec spec;
    spec.name = name;
    spec.width = width;
    spec.height = height;
    spec.referenceX = x;
    spec.referenceY = y;
    spec.sourceWidth = source.width();
    spec.sourceHeight = source.height();
    // 先让聚合检查（R8），通过了再保存图像内容：不把注定被拒绝的图像写进内容仓库
    if (auto st = recipe->checkNewMatchTemplate(spec); !st) return st.error();
    auto stored = templateStore_->storeCrop(source, x, y, width, height);
    if (!stored) return stored.error();
    spec.sha256 = stored.value().sha256;
    spec.bytes = stored.value().bytes;

    auto id = recipe->addMatchTemplate(std::move(spec));
    if (!id) return id.error();
    if (auto st = recipes_.save(*recipe); !st) return st.error();
    events_.publishAll(recipe->pullEvents());
    return id.value();
}

Status AuthoringService::renameMatchTemplate(const std::string& recipeId, const std::string& templateId,
                                             const std::string& name) {
    return modify(recipeId, [&](Recipe& r) { return r.renameMatchTemplate(templateId, name); });
}

Status AuthoringService::removeMatchTemplate(const std::string& recipeId, const std::string& templateId) {
    // 只删引用，不删内容：已发布的方案版本可能还在用同一份内容
    return modify(recipeId, [&](Recipe& r) { return r.removeMatchTemplate(templateId); });
}

Result<std::string> AuthoringService::publish(const std::string& recipeId) {
    auto recipe = recipes_.find(recipeId);
    if (!recipe) return notFound(recipeId).error();

    // recipe.publish() 生成另一个聚合 RecipeVersion；两个聚合分别保存。
    auto version = recipe->publish();
    if (!version) return version.error();
    if (auto st = versions_.save(version.value()); !st) return st.error();
    if (auto st = recipes_.save(*recipe); !st) return st.error();
    events_.publishAll(recipe->pullEvents());
    return version.value().id().str();
}

Status AuthoringService::deprecateVersion(const std::string& versionId) {
    auto id = RecipeVersionId::parse(versionId);
    if (!id) return id.error();
    auto version = versions_.find(id.value());
    if (!version) return Status::failure("authoring.version_not_found", "找不到方案版本 " + versionId);
    if (auto st = version->deprecate(); !st) return st;
    if (auto st = versions_.save(*version); !st) return st;
    events_.publishAll(version->pullEvents());
    return Status::success();
}

Result<DraftView> AuthoringService::getDraft(const std::string& recipeId) const {
    auto recipe = recipes_.find(recipeId);
    if (!recipe) return notFound(recipeId).error();
    DraftView view{recipe->id(), recipe->name(), {}, {}, {}};
    for (const auto& n : recipe->nodes()) {
        view.nodes.push_back(DraftNodeView{n.id, n.op.id, n.params, describeRoi(n.roi)});
    }
    for (const auto& e : recipe->edges()) view.edges.push_back({e.from.str(), e.to.str()});
    view.templates = toDocument(recipe->templates());
    return view;
}

Result<RecipeVersionDocument> AuthoringService::getVersionDocument(const std::string& versionId) const {
    auto id = RecipeVersionId::parse(versionId);
    if (!id) return id.error();
    auto version = versions_.find(id.value());
    if (!version) {
        return Result<RecipeVersionDocument>::failure("authoring.version_not_found", "找不到方案版本 " + versionId);
    }
    // 领域模型 → 发布语言
    RecipeVersionDocument doc;
    doc.versionId = version->id().str();
    doc.recipeName = version->recipeName();
    for (const auto& n : version->nodes()) {
        doc.nodes.push_back(RecipeDocumentNode{n.id, n.op.id, n.op.majorVersion, n.params, version->resolveRoi(n)});
    }
    for (const auto& e : version->edges()) doc.edges.push_back({e.from.str(), e.to.str()});
    doc.templates = toDocument(version->templates());
    return doc;
}

Result<RecipeVersionDocument> AuthoringService::getDraftDocument(const std::string& recipeId) const {
    auto recipe = recipes_.find(recipeId);
    if (!recipe) return notFound(recipeId).error();
    // 借用 RecipeVersion 的 ROI 展开逻辑，但不发布、不保存
    RecipeVersion draft(RecipeVersionId{recipe->id(), 0}, recipe->name(), recipe->nodes(), recipe->edges(),
                        recipe->regions());
    RecipeVersionDocument doc;
    doc.versionId = recipe->id() + "@draft";
    doc.recipeName = recipe->name();
    for (const auto& n : draft.nodes()) {
        doc.nodes.push_back(RecipeDocumentNode{n.id, n.op.id, n.op.majorVersion, n.params, draft.resolveRoi(n)});
    }
    for (const auto& e : draft.edges()) doc.edges.push_back({e.from.str(), e.to.str()});
    doc.templates = toDocument(recipe->templates());
    return doc;
}

std::vector<std::string> AuthoringService::listRecipes() const { return recipes_.listIds(); }

}  // namespace vision::authoring
