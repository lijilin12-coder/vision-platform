#pragma once

#include <optional>
#include <vector>

#include "vision/authoring/domain/recipe.h"
#include "vision/authoring/domain/recipe_version.h"
#include "vision/common/result.h"

namespace vision::authoring {

// 仓储接口定义在领域层；实现在 adapters/persistence（JSON 文件）或测试中（内存）。
// 每个聚合一个仓储，只保存和读取完整的聚合。

class RecipeRepository {
public:
    virtual ~RecipeRepository() = default;
    virtual Status save(const Recipe& recipe) = 0;
    virtual std::optional<Recipe> find(const RecipeId& id) = 0;
    virtual std::vector<RecipeId> listIds() = 0;
};

class RecipeVersionRepository {
public:
    virtual ~RecipeVersionRepository() = default;
    virtual Status save(const RecipeVersion& version) = 0;
    virtual std::optional<RecipeVersion> find(const RecipeVersionId& id) = 0;
};

}  // namespace vision::authoring
