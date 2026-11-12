#pragma once

// JSON 文件仓储（ADR-20：聚合用 JSON 文件，每个聚合一个文件）。
// 适配器可以依赖领域层：它实现的是领域层定义的仓储接口。

#include <filesystem>
#include <string>

#include "vision/authoring/api.h"
#include "vision/authoring/domain/repositories.h"

namespace vision::persistence {

// 目录结构：
//   <root>/recipes/<recipeId>.json
//   <root>/recipe-versions/<recipeId>/v<n>.json
class JsonRecipeRepository final : public authoring::RecipeRepository {
public:
    explicit JsonRecipeRepository(std::filesystem::path root);
    Status save(const authoring::Recipe& recipe) override;
    std::optional<authoring::Recipe> find(const authoring::RecipeId& id) override;
    std::vector<authoring::RecipeId> listIds() override;

private:
    std::filesystem::path dir_;
};

class JsonRecipeVersionRepository final : public authoring::RecipeVersionRepository {
public:
    explicit JsonRecipeVersionRepository(std::filesystem::path root);
    Status save(const authoring::RecipeVersion& version) override;
    std::optional<authoring::RecipeVersion> find(const authoring::RecipeVersionId& id) override;

private:
    std::filesystem::path dir_;
};

// 发布语言（方案版本文档）的 JSON 序列化，用于导出和查看。
std::string toJson(const authoring::RecipeVersionDocument& doc, int indent = 2);
Result<authoring::RecipeVersionDocument> recipeDocumentFromJson(const std::string& text);

}  // namespace vision::persistence
