#pragma once

#include <string>

#include "vision/common/domain_event.h"

namespace vision::authoring {

// 编辑草稿的操作（加节点、连线……）不发布领域事件：其他上下文不关心编辑过程。

struct RecipeCreated final : DomainEvent {
    std::string recipeId;
    std::string recipeName;
    RecipeCreated(std::string id, std::string n) : recipeId(std::move(id)), recipeName(std::move(n)) {}
    const char* name() const override { return "RecipeCreated"; }
};

struct RecipeVersionPublished final : DomainEvent {
    std::string recipeVersionId;
    explicit RecipeVersionPublished(std::string id) : recipeVersionId(std::move(id)) {}
    const char* name() const override { return "RecipeVersionPublished"; }
};

struct RecipeVersionDeprecated final : DomainEvent {
    std::string recipeVersionId;
    explicit RecipeVersionDeprecated(std::string id) : recipeVersionId(std::move(id)) {}
    const char* name() const override { return "RecipeVersionDeprecated"; }
};

}  // namespace vision::authoring
