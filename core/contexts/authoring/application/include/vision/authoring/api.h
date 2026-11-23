#pragma once

// BC-2 方案编排的公开接口。其他上下文、适配器、应用只能包含这个头文件，
// 不能包含 vision/authoring/domain/ 下的任何头文件（构建系统会阻止）。

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"
#include "vision/kernel/geometry.h"
#include "vision/kernel/image.h"
#include "vision/kernel/signature.h"

namespace vision::catalog {
class OperatorCatalog;
}

namespace vision::authoring {

// 仓储接口在领域层，这里只做前置声明；由组装根（apps/）传入具体实现。
class RecipeRepository;
class RecipeVersionRepository;

// ===== 发布语言：方案版本文档（docs/ddd/04-context-map.md，BC-2 → BC-4 / BC-3）=====
// 下游上下文只认这个格式。编排内部怎么变都不影响它，只要格式不变。
// 注意：这里已经没有画布布局，具名区域也已展开成节点自己的 ROI。

// 0.2：新增 templates（匹配模板）。0.1 的文档没有这一段，照样能读。
inline constexpr const char* kRecipeDocumentSchemaVersion = "0.2";
inline bool isSupportedRecipeDocumentSchema(const std::string& version) { return version == "0.1" || version == "0.2"; }

struct RecipeDocumentNode {
    std::string id;
    std::string operatorId;
    int operatorMajor = 1;
    ParamMap params;
    std::optional<Roi> roi;
};

struct RecipeDocumentEdge {
    std::string from;  // "n1.image"
    std::string to;
};

// 匹配模板：对内容的引用（哈希）+ 它在样图上的位置。图像内容本身不在文档里，按哈希到内容仓库取。
struct RecipeDocumentTemplate {
    std::string id;       // 节点的模板参数里写的就是它
    std::string name;
    std::string sha256;
    long long bytes = 0;
    int width = 0, height = 0;
    double referenceX = 0, referenceY = 0;
    int sourceWidth = 0, sourceHeight = 0;
};

struct RecipeVersionDocument {
    std::string schemaVersion = kRecipeDocumentSchemaVersion;
    std::string versionId;
    std::string recipeName;
    std::vector<RecipeDocumentNode> nodes;
    std::vector<RecipeDocumentEdge> edges;
    std::vector<RecipeDocumentTemplate> templates;
};

// ===== 端口：保存匹配模板的图像内容 =====
// 从样图上裁出一块、保存起来，返回内容的哈希。同样的内容保存多少次，结果都一样（内容寻址，ADR-20）。
struct StoredContent {
    std::string sha256;
    long long bytes = 0;
};

class TemplateStore {
public:
    virtual ~TemplateStore() = default;
    virtual Result<StoredContent> storeCrop(const Image& source, int x, int y, int width, int height) = 0;
};

// ===== 查询用 DTO =====

struct DraftNodeView {
    std::string id;
    std::string operatorId;
    ParamMap params;
    std::string roi;  // 文字描述
};

struct DraftView {
    std::string recipeId;
    std::string name;
    std::vector<DraftNodeView> nodes;
    std::vector<RecipeDocumentEdge> edges;
    std::vector<RecipeDocumentTemplate> templates;
};

// ===== 应用服务 =====
// 每个命令都是：取聚合 → 调用领域对象 → 保存 → 发布事件。本身不包含业务规则。

class AuthoringService {
public:
    AuthoringService(RecipeRepository& recipes, RecipeVersionRepository& versions,
                     const catalog::OperatorCatalog& catalog, EventPublisher& events);

    Result<std::string> createRecipe(const std::string& name);
    Result<std::string> addNode(const std::string& recipeId, const std::string& operatorId);
    Status removeNode(const std::string& recipeId, const std::string& nodeId);
    Status connect(const std::string& recipeId, const std::string& from, const std::string& to);
    Status disconnect(const std::string& recipeId, const std::string& from, const std::string& to);
    Status setParam(const std::string& recipeId, const std::string& nodeId, const std::string& param,
                    const ParamValue& value);
    Status setRoi(const std::string& recipeId, const std::string& nodeId, const Roi& roi);
    Status clearRoi(const std::string& recipeId, const std::string& nodeId);
    Result<std::string> defineNamedRegion(const std::string& recipeId, const std::string& name, const Roi& roi);
    Status useNamedRegion(const std::string& recipeId, const std::string& nodeId, const std::string& regionId);
    Status removeNamedRegion(const std::string& recipeId, const std::string& regionId);

    // 匹配模板（docs/ddd/09-match-template-and-locating.md）。
    // 要创建模板，组装根必须先用 useTemplateStore 接上保存图像内容的地方。
    void useTemplateStore(TemplateStore& store) { templateStore_ = &store; }
    // 从样图 source 上框出 (x, y, width, height) 这一块作为模板，返回模板 ID
    Result<std::string> createMatchTemplate(const std::string& recipeId, const std::string& name, const Image& source,
                                            int x, int y, int width, int height);
    Status renameMatchTemplate(const std::string& recipeId, const std::string& templateId, const std::string& name);
    Status removeMatchTemplate(const std::string& recipeId, const std::string& templateId);

    // 发布，返回方案版本 ID
    Result<std::string> publish(const std::string& recipeId);
    Status deprecateVersion(const std::string& versionId);

    Result<DraftView> getDraft(const std::string& recipeId) const;
    Result<RecipeVersionDocument> getVersionDocument(const std::string& versionId) const;
    // 调试运行用：把当前草稿按发布语言的格式导出（versionId 为 "<recipeId>@draft"），不检查发布规则。
    Result<RecipeVersionDocument> getDraftDocument(const std::string& recipeId) const;
    std::vector<std::string> listRecipes() const;

private:
    template <typename Fn>
    Status modify(const std::string& recipeId, Fn&& fn);

    RecipeRepository& recipes_;
    RecipeVersionRepository& versions_;
    const catalog::OperatorCatalog& catalog_;
    EventPublisher& events_;
    TemplateStore* templateStore_ = nullptr;
};

}  // namespace vision::authoring
