#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/result.h"

namespace vision::release {

// 方案包的导入规则（H-06，docs/ddd/12-recipe-package.md）。
//
// 这里只有规则，不碰文件：输入是“包里声称有什么”和“本机现在有什么”，输出是“可以导入，要新增这些”或者“不行，因为……”。
// 读文件、核对校验和、真正写进仓库，是应用层和适配器的事。

// 包里的东西（从清单里来）
struct PackageFacts {
    std::string schema;           // 清单的格式版本
    std::string recipeVersionId;  // 包里的方案版本
    std::string recipeSha256;     // 方案版本文档的内容哈希
    std::vector<std::pair<std::string, int>> operators;          // 用到的算子：ID、主版本号
    std::vector<std::pair<std::string, std::string>> models;     // 带来的模型版本：ID、文件的内容哈希
};

// 本机现在有的东西
struct LocalInventory {
    std::map<std::string, int> operators;                 // 本机的算子：ID → 主版本号
    std::optional<std::string> recipeSha256;              // 本机已经有同 ID 的方案版本时：它的内容哈希
    std::map<std::string, std::string> modelSha256;       // 本机已经有的同 ID 模型版本：ID → 内容哈希
};

// 导入要做的事
struct ImportPlan {
    bool addRecipeVersion = false;           // 为假表示本机已经有一模一样的
    std::vector<std::string> modelsToAdd;
    std::vector<std::string> modelsAlreadyHere;
    bool nothingToDo() const { return !addRecipeVersion && modelsToAdd.empty(); }
};

inline constexpr const char* kRecipePackageSchema = "vision.recipe-package/0.1";

// 规则：
//   K1 清单的格式版本必须是本平台认识的
//   K2 包里用到的每个算子，本机都必须有，而且主版本号相同（D5-1）。缺一个就整个拒绝
//   K3 同 ID 不同内容是冲突，拒绝：方案版本、模型版本都是不可变的，同一个 ID 不可能指两样东西
//   K4 重复导入不出错：本机已经有一模一样的，就什么都不用做（幂等）
Result<ImportPlan> planImport(const PackageFacts& package, const LocalInventory& local);

}  // namespace vision::release
