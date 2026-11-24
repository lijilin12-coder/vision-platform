#include "vision/release/domain/recipe_package.h"

namespace vision::release {

Result<ImportPlan> planImport(const PackageFacts& package, const LocalInventory& local) {
    using R = Result<ImportPlan>;
    // K1
    if (package.schema != kRecipePackageSchema) {
        return R::failure("release.package_schema", "不认识这种方案包（格式版本 " + package.schema + "）。它可能来自更新的平台");
    }
    if (package.recipeVersionId.empty() || package.recipeSha256.empty()) {
        return R::failure("release.package_invalid", "方案包的清单不完整：没有方案版本");
    }
    // K2：把缺的全列出来，而不是遇到第一个就停——用的人想一次知道差哪些
    std::string missing;
    for (const auto& [id, major] : package.operators) {
        const auto have = local.operators.find(id);
        if (have == local.operators.end()) {
            missing += (missing.empty() ? "" : "；") + id + "（本平台没有）";
        } else if (have->second != major) {
            missing += (missing.empty() ? "" : "；") + id + "（需要主版本 " + std::to_string(major) + "，本平台是 " +
                       std::to_string(have->second) + "）";
        }
    }
    if (!missing.empty()) return R::failure("release.package_missing_operator", "本平台缺少方案包需要的算子：" + missing);

    ImportPlan plan;
    // K3、K4：方案版本
    if (!local.recipeSha256) {
        plan.addRecipeVersion = true;
    } else if (*local.recipeSha256 != package.recipeSha256) {
        return R::failure("release.package_conflict",
                          "本平台已经有方案版本 " + package.recipeVersionId + "，但内容和包里的不一样。方案版本是不可变的，同一个 ID 不应该有两种内容");
    }
    // K3、K4：模型版本
    for (const auto& [id, sha256] : package.models) {
        const auto have = local.modelSha256.find(id);
        if (have == local.modelSha256.end()) {
            plan.modelsToAdd.push_back(id);
        } else if (have->second != sha256) {
            return R::failure("release.package_conflict",
                              "本平台已经有模型版本 " + id + "，但内容和包里的不一样。模型版本是不可变的，同一个 ID 不应该有两种内容");
        } else {
            plan.modelsAlreadyHere.push_back(id);
        }
    }
    return plan;
}

}  // namespace vision::release
