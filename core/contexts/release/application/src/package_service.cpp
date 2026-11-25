#include <algorithm>
#include <set>

#include "vision/common/id.h"
#include "vision/release/api.h"
#include "vision/release/domain/recipe_package.h"

namespace vision::release {

namespace {

// 清单 → 规则要看的那几项
PackageFacts factsOf(const PackageManifest& m) {
    PackageFacts f;
    f.schema = m.schema;
    f.recipeVersionId = m.recipeVersionId;
    f.recipeSha256 = m.recipeSha256;
    f.operators = m.operators;
    for (const auto& model : m.models) f.models.emplace_back(model.id, model.sha256);
    return f;
}

}  // namespace

PackageService::PackageService(const RecipeVersionSource& recipes, ImportedRecipes& imported, PackageExchange& exchange,
                               const OperatorInventory& operators, ModelArchive& models, ContentArchive& content,
                               EventPublisher& events)
    : recipes_(recipes), imported_(imported), exchange_(exchange), operators_(operators), models_(models), content_(content),
      events_(events) {}

Result<PackageFile> PackageService::build(const std::string& recipeVersionId, ExportReport& report) {
    using R = Result<PackageFile>;
    auto doc = recipes_.find(recipeVersionId);
    if (!doc) return doc.error();

    PackageFile package;
    package.recipe = doc.value();
    PackageManifest& m = package.manifest;
    m.schema = kRecipePackageSchema;
    m.packageId = newUuid();
    m.recipeVersionId = doc.value().versionId;
    m.recipeName = doc.value().recipeName;
    m.recipeSha256 = exchange_.fingerprint(doc.value());

    // 用到的算子，以及节点上的模型参数指向的模型版本
    std::set<std::pair<std::string, int>> usedOperators;
    std::set<std::string> usedModels;
    for (const auto& node : doc.value().nodes) {
        usedOperators.emplace(node.operatorId, node.operatorMajor);
        const auto facts = operators_.find(node.operatorId);
        if (!facts) return R::failure("release.package_missing_operator", "本平台没有算子 " + node.operatorId + "，无法导出用到它的方案");
        for (const auto& param : facts->modelParams) {
            const auto value = node.params.find(param);
            if (value == node.params.end() || !std::holds_alternative<std::string>(value->second)) continue;
            if (const auto& id = std::get<std::string>(value->second); !id.empty()) usedModels.insert(id);
        }
    }
    m.operators.assign(usedOperators.begin(), usedOperators.end());

    for (const auto& t : doc.value().templates) {
        auto bytes = content_.get(t.sha256);
        if (!bytes) return R::failure(bytes.error().code, "匹配模板“" + t.name + "”的图像读不出来：" + bytes.error().message);
        report.contentBytes += static_cast<long long>(bytes.value().size());
        m.templates.push_back({t.id, t.name, t.sha256, static_cast<long long>(bytes.value().size())});
        package.contents[t.sha256] = std::move(bytes).value();
    }
    for (const auto& id : usedModels) {
        auto model = models_.find(id);
        if (!model) return R::failure(model.error().code, "方案用到的模型版本 " + id + " 找不到");
        if (!model.value().published) {
            return R::failure("release.package_unpublished_model",
                              "方案用到的模型版本 " + id + " 还没有发布。没发布的模型带到另一台机器上也不能正式使用，请先发布");
        }
        auto bytes = content_.get(model.value().entry.sha256);
        if (!bytes) return R::failure(bytes.error().code, "模型版本 " + id + " 的文件读不出来：" + bytes.error().message);
        report.contentBytes += static_cast<long long>(bytes.value().size());
        m.models.push_back(model.value().entry);
        package.contents[model.value().entry.sha256] = std::move(bytes).value();
    }

    report.packageId = m.packageId;
    report.recipeVersionId = m.recipeVersionId;
    report.templates = static_cast<int>(m.templates.size());
    report.models = static_cast<int>(m.models.size());
    return package;
}

Result<ExportReport> PackageService::exportPackage(const std::string& recipeVersionId, const std::string& filePath) {
    ExportReport report;
    auto package = build(recipeVersionId, report);
    if (!package) return package.error();
    if (auto st = exchange_.write(filePath, package.value()); !st) return st.error();
    return report;
}

Result<ExportReport> PackageService::exportRelease(const std::string& releaseId, const std::string& filePath) {
    using R = Result<ExportReport>;
    if (releases_ == nullptr) return R::failure("release.no_bundles", "这个程序没有接上上线管理，不能导出上线包文件");
    auto bundled = releases_->bundle(releaseId);  // B1
    if (!bundled) return bundled.error();
    ExportReport report;
    auto package = build(bundled.value().recipeVersionId, report);
    if (!package) return package.error();
    package.value().manifest.release = std::move(bundled).value();
    if (auto st = exchange_.write(filePath, package.value()); !st) return st.error();
    return report;
}

Result<BundleImportReport> PackageService::importRelease(const std::string& filePath) {
    using R = Result<BundleImportReport>;
    if (releases_ == nullptr) return R::failure("release.no_bundles", "这个程序没有接上上线管理，不能导入上线包文件");
    auto read = exchange_.read(filePath);
    if (!read) return read.error();
    const auto& bundled = read.value().manifest.release;
    if (!bundled) {
        return R::failure("release.bundle_no_release",
                          "这个文件是方案包，里面没有上线包。方案包用 package import 导入；要带着验收记录上线，请在方案构建平台上用 release export 导出");
    }
    if (bundled->recipeVersionId != read.value().manifest.recipeVersionId) {
        return R::failure("release.bundle_invalid", "上线包文件里的上线包和它带的方案版本对不上");
    }
    // 先接收不了的就什么都不导入：按 B1 预先看一眼（真正的检查在 adopt 里还会再做一遍）
    if (!bundled->acceptance.passed) return R::failure("release.bundle_not_accepted", "上线包文件里的上线包没有通过验收检查，不接收");

    BundleImportReport report;
    {
        auto imported = importFile(read.value());
        if (!imported) return imported.error();
        report.package = std::move(imported).value();
    }
    auto added = releases_->adopt(*bundled);
    if (!added) return added.error();
    report.releaseId = bundled->releaseId;
    report.releaseAdded = added.value();
    report.acceptance = bundled->acceptance;
    return report;
}



Result<PackageManifest> PackageService::inspect(const std::string& filePath) {
    auto package = exchange_.read(filePath);
    if (!package) return package.error();
    return package.value().manifest;
}

Result<ImportReport> PackageService::importPackage(const std::string& filePath) {
    // 读文件。适配器已经核对过结构和每一份内容的校验和
    auto read = exchange_.read(filePath);
    if (!read) return read.error();
    // 上线包文件也可以当方案包导入：只取方案版本那一部分，里面的上线包不管
    return importFile(read.value());
}

Result<ImportReport> PackageService::importFile(const PackageFile& package) {
    using R = Result<ImportReport>;
    const PackageManifest& m = package.manifest;

    std::lock_guard<std::mutex> lock(mutex_);

    // ---- 第一步：只检查，不写任何东西 ----
    // 清单和方案文档说的必须是同一个方案版本
    if (package.recipe.versionId != m.recipeVersionId) {
        return R::failure("release.package_invalid", "方案包的清单和它里面的方案版本对不上");
    }
    // 清单里列出的每一份内容，包里都得真的有
    for (const auto& t : m.templates) {
        if (package.contents.count(t.sha256) == 0) return R::failure("release.package_invalid", "方案包里缺少匹配模板“" + t.name + "”的图像");
    }
    for (const auto& model : m.models) {
        if (package.contents.count(model.sha256) == 0) return R::failure("release.package_invalid", "方案包里缺少模型版本 " + model.id + " 的文件");
    }
    // 方案文档里引用的每个模板，清单里都得有
    for (const auto& t : package.recipe.templates) {
        if (std::none_of(m.templates.begin(), m.templates.end(), [&](const PackageTemplateEntry& e) { return e.sha256 == t.sha256; })) {
            return R::failure("release.package_invalid", "方案用到的匹配模板“" + t.name + "”不在方案包里");
        }
    }

    // 本机现在有什么
    LocalInventory local;
    for (const auto& [id, major] : m.operators) {
        if (const auto facts = operators_.find(id)) local.operators[id] = facts->majorVersion;
    }
    if (auto existing = recipes_.find(m.recipeVersionId)) local.recipeSha256 = exchange_.fingerprint(existing.value());
    for (const auto& model : m.models) {
        if (auto existing = models_.find(model.id)) local.modelSha256[model.id] = existing.value().entry.sha256;
    }
    // 规则 K1～K4
    auto plan = planImport(factsOf(m), local);
    if (!plan) return plan.error();

    // ---- 第二步：写。顺序是“被引用的先写”：内容 → 模型 → 方案版本。
    // 中途失败的话，留下的只是没人引用的内容和模型（它们本身是完整、正确的），再导入一次就能接着完成 ----
    for (const auto& t : m.templates) {
        if (auto st = content_.put(package.contents.at(t.sha256), t.sha256); !st) return st.error();
    }
    for (const auto& id : plan.value().modelsToAdd) {
        const auto entry = std::find_if(m.models.begin(), m.models.end(), [&](const PackageModelEntry& e) { return e.id == id; });
        if (auto st = models_.add(*entry, package.contents.at(entry->sha256)); !st) return st.error();
    }
    if (plan.value().addRecipeVersion) {
        if (auto st = imported_.save(package.recipe); !st) return st.error();
    }

    ImportReport report;
    report.packageId = m.packageId;
    report.recipeVersionId = m.recipeVersionId;
    report.recipeName = m.recipeName;
    report.recipeAdded = plan.value().addRecipeVersion;
    report.modelsAdded = plan.value().modelsToAdd;
    report.modelsAlreadyHere = plan.value().modelsAlreadyHere;
    report.templates = static_cast<int>(m.templates.size());
    if (!report.nothingNew()) events_.publish(std::make_shared<RecipePackageImported>(m.packageId, m.recipeVersionId));
    return report;
}

}  // namespace vision::release
