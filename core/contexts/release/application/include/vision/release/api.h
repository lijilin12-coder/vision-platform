#pragma once

// BC-4 上线管理的公开接口（POC-9 需要的最小部分）。

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "vision/authoring/api.h"  // 只用到发布语言 RecipeVersionDocument
#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::release {

class ReleaseRepository;  // 领域层的仓储接口，由组装根传入实现
class GoldenSetRepository;

// ===== 发布语言：上线包清单（docs/ddd/04-context-map.md，BC-4 → BC-5）=====
// 工位只认上线包，不关心它是怎么组装出来的。
struct ReleaseManifest {
    std::string releaseId;
    std::string recipeVersionId;
    std::map<std::string, std::string> lockedModels;  // 模型槽位 → 模型版本 ID
    bool withdrawn = false;
};

// 上线包的全部内容：清单 + 方案版本文档。加载上线包时用。
struct ReleaseContent {
    ReleaseManifest manifest;
    authoring::RecipeVersionDocument recipe;
};

// 端口：按 ID 取得方案版本文档。
// 在方案构建平台上由方案编排提供；在产线运行平台上，将来由导入的上线包文件提供。
// 上线管理因此不直接依赖方案编排的服务。
class RecipeVersionSource {
public:
    virtual ~RecipeVersionSource() = default;
    virtual Result<authoring::RecipeVersionDocument> find(const std::string& recipeVersionId) const = 0;
};

// 端口：一个方案版本现在能不能被检测引擎加载——算子齐不齐、匹配模板读不读得出来、用到的模型有没有发布。
// 创建上线包之前问一次：不能加载的方案版本做成上线包没有意义，到了工位上也只会加载失败。
class LoadCheck {
public:
    virtual ~LoadCheck() = default;
    virtual Status canLoad(const authoring::RecipeVersionDocument& recipe) = 0;
};

// 应用服务。可以从多个线程调用（工位在后台线程里加载上线包）。
class ReleaseService {
public:
    ReleaseService(ReleaseRepository& releases, const RecipeVersionSource& recipes, EventPublisher& events);

    // 用一个已发布的方案版本创建上线包，返回上线包 ID。方案版本不存在时被拒绝；
    // 配置了 LoadCheck 时，不能加载的方案版本（例如用了还没发布的模型）也被拒绝。
    Result<std::string> createRelease(const std::string& recipeVersionId);
    void useLoadCheck(LoadCheck& check) { loadCheck_ = &check; }
    Status withdraw(const std::string& releaseId);

    Result<ReleaseManifest> getManifest(const std::string& releaseId) const;
    Result<ReleaseContent> getContent(const std::string& releaseId) const;
    std::vector<std::string> listReleases() const;

private:
    ReleaseRepository& releases_;
    const RecipeVersionSource& recipes_;
    EventPublisher& events_;
    LoadCheck* loadCheck_ = nullptr;
    mutable std::mutex mutex_;  // 保护仓储：仓储的实现不要求线程安全
};

// ===================== 基准图集与评估 =====================

struct GoldenCaseView {
    std::string id;
    std::string imageRef;
    bool truthNg = false;  // 人确认的真实判定：是否不合格
    std::string sampleId;  // 这幅图像是哪个样本；没有接上数据上下文时为空
};

// 端口：样本登记处（接到 BC-9 数据上）。基准图集的用例引用的是样本，这样才能检查规则 N1
// （docs/ddd/08-platforms-and-training.md §3）。
class SampleRegistry {
public:
    virtual ~SampleRegistry() = default;
    // 这幅图像是哪个样本（按内容识别；没见过的图像会被登记成新的样本）。truthNg 是人确认的真实判定。
    virtual Result<std::string> sampleOf(const std::string& imageRef, bool truthNg) = 0;
    // 把这些样本的用途定为“留作基准”。其中有已经用于训练的 → 整体拒绝（N1）。
    virtual Status reserveForBenchmark(const std::vector<std::string>& sampleIds) = 0;
};

struct CriteriaView {
    int maxMisses = 0;
    double maxFalseAlarmRate = 0.0;
    int maxErrors = 0;
};

// 创建基准图集时提交的内容
struct GoldenSetDefinition {
    std::string name;
    std::vector<GoldenCaseView> cases;
    CriteriaView criteria;
};

struct GoldenSetView {
    std::string id;
    std::string name;
    std::vector<GoldenCaseView> cases;
    CriteriaView criteria;
};

// 一个用例上的检测结果。verdict 是检测给出的判定："OK"、"NG"，其他一律按 ERROR 处理。
struct CaseResultView {
    std::string caseId;
    std::string verdict;
    double durationMs = 0;
};

// 评估结果（docs/ddd/08-platforms-and-training.md §4：这套规则只有一份，三处共用）
struct EvaluationView {
    int total = 0;
    int truthOk = 0;
    int truthNg = 0;
    int misses = 0;       // 漏检
    int falseAlarms = 0;  // 误报
    int errors = 0;
    double falseAlarmRate = 0;
    double meanMs = 0;
    double maxMs = 0;
    bool meetsCriteria = false;  // 是否达到这个基准图集的验收标准
    std::vector<std::string> wrongCaseIds;
};

class GoldenSetService {
public:
    GoldenSetService(GoldenSetRepository& sets, EventPublisher& events);

    // 创建并冻结一个基准图集，返回它的 ID。冻结之后不能修改。
    // 接上了样本登记处时：每个用例登记成样本，并且把它们的用途定为“留作基准”；
    // 其中有已经用于训练的图像，冻结被拒绝（N1）。
    Result<std::string> createGoldenSet(const GoldenSetDefinition& definition);
    void useSampleRegistry(SampleRegistry& registry) { sampleRegistry_ = &registry; }
    Result<GoldenSetView> getGoldenSet(const std::string& goldenSetId) const;
    std::vector<std::string> listGoldenSets() const;

    // 对照真实判定评估一组检测结果。results 可以只覆盖图集的一部分。
    Result<EvaluationView> evaluate(const std::string& goldenSetId, const std::vector<CaseResultView>& results) const;

    // a 是否比 b 好：ERROR 少的好，其次漏检少的好，再其次误报少的好。
    static bool betterThan(const EvaluationView& a, const EvaluationView& b);

private:
    GoldenSetRepository& sets_;
    EventPublisher& events_;
    SampleRegistry* sampleRegistry_ = nullptr;
    mutable std::mutex mutex_;
};

// =====================================================================================
// 方案包（REL-02，H-06，docs/ddd/12-recipe-package.md）
// 把一个方案版本连同它用到的匹配模板、模型打成一个文件，带到另一台机器上导入。
// =====================================================================================

// ===== 发布语言：方案包的清单 =====
// 它就是包文件里 manifest.json 的内容；两台机器之间靠它说话。

struct PackageTemplateEntry {
    std::string id, name, sha256;
    long long bytes = 0;
};

struct PackageModelEntry {
    std::string id;  // 模型版本 ID，例如 "surface-anomaly@v1"
    std::string sha256;
    long long bytes = 0;
    // 模型契约和来源：导入的一方靠它们登记这个模型
    std::string kind, output;
    int inputWidth = 0, inputHeight = 0, inputChannels = 1;
    std::map<std::string, double> numbers;
    std::string provenanceKind, trainingJobId, datasetVersionId, split, backend;
};

struct PackageManifest {
    std::string schema;
    std::string packageId;        // 每次导出一个新的 ID
    std::string recipeVersionId, recipeName;
    std::string recipeSha256;     // 方案版本文档的内容哈希
    std::vector<std::pair<std::string, int>> operators;  // 用到的算子：ID、主版本号
    std::vector<PackageTemplateEntry> templates;
    std::vector<PackageModelEntry> models;
};

// 一个方案包的全部内容（已经读进内存、校验和都核对过）
struct PackageFile {
    PackageManifest manifest;
    authoring::RecipeVersionDocument recipe;
    std::map<std::string, std::vector<unsigned char>> contents;  // 内容哈希 → 字节（模板图像、模型文件）
};

// ===== 端口 =====

// 包文件的读写（先只有“一个文件”这一种传输方式；传输方式是适配器的事）。
class PackageExchange {
public:
    virtual ~PackageExchange() = default;
    virtual Status write(const std::string& filePath, const PackageFile& package) = 0;
    // 读出来的同时核对：结构完整、每一份内容的哈希和清单一致、方案文档的哈希和清单一致。任何一处不对都返回失败
    virtual Result<PackageFile> read(const std::string& filePath) = 0;
    // 方案版本文档的内容哈希（两台机器上对同一份文档必须算出同一个数）
    virtual std::string fingerprint(const authoring::RecipeVersionDocument& recipe) const = 0;
};

// 本机有哪些算子，以及一个算子的哪些参数是“模型”
struct OperatorFacts {
    int majorVersion = 1;
    std::vector<std::string> modelParams;
};
class OperatorInventory {
public:
    virtual ~OperatorInventory() = default;
    virtual std::optional<OperatorFacts> find(const std::string& operatorId) const = 0;
};

// 模型仓库在方案包这件事上要做的两件事
struct ArchivedModel {
    PackageModelEntry entry;
    bool published = false;
};
class ModelArchive {
public:
    virtual ~ModelArchive() = default;
    virtual Result<ArchivedModel> find(const std::string& modelVersionId) = 0;
    // 把包里带来的模型登记进模型仓库，ID 不变、状态是“已发布”（它在导出的那一方已经发布过了）
    virtual Status add(const PackageModelEntry& entry, const std::vector<unsigned char>& bytes) = 0;
};

// 按内容哈希存取二进制内容（匹配模板的图像、模型文件）
class ContentArchive {
public:
    virtual ~ContentArchive() = default;
    virtual Result<std::vector<unsigned char>> get(const std::string& sha256) = 0;
    virtual Status put(const std::vector<unsigned char>& bytes, const std::string& expectedSha256) = 0;
};

// 导入进来的方案版本放在哪里。
// 导入的方案版本不进方案编排（那边是可以编辑的方案）；它只是一份只读的文档，给上线包和检测引擎用。
// 产线运行平台上没有方案编排，方案版本全都来自这里。
class ImportedRecipes {
public:
    virtual ~ImportedRecipes() = default;
    virtual Status save(const authoring::RecipeVersionDocument& recipe) = 0;
    virtual std::optional<authoring::RecipeVersionDocument> find(const std::string& recipeVersionId) const = 0;
    virtual std::vector<std::string> listIds() const = 0;
};

// ===== 应用服务 =====

struct ExportReport {
    std::string packageId, recipeVersionId;
    int templates = 0, models = 0;
    long long contentBytes = 0;
};

struct ImportReport {
    std::string packageId, recipeVersionId, recipeName;
    bool recipeAdded = false;                 // 为假：本机已经有一模一样的
    std::vector<std::string> modelsAdded, modelsAlreadyHere;
    int templates = 0;
    bool nothingNew() const { return !recipeAdded && modelsAdded.empty(); }
};

class PackageService {
public:
    PackageService(const RecipeVersionSource& recipes, ImportedRecipes& imported, PackageExchange& exchange,
                   const OperatorInventory& operators, ModelArchive& models, ContentArchive& content, EventPublisher& events);

    // 导出：方案版本必须存在；它用到的模型必须已经发布（没发布的模型带过去也不能正式使用）。
    Result<ExportReport> exportPackage(const std::string& recipeVersionId, const std::string& filePath);
    // 导入：先把所有规则检查完（K1～K4），再开始写；规则不过，什么都不会留下。重复导入不出错。
    Result<ImportReport> importPackage(const std::string& filePath);
    // 只看不导入：清单里有什么
    Result<PackageManifest> inspect(const std::string& filePath);

private:
    const RecipeVersionSource& recipes_;
    ImportedRecipes& imported_;
    PackageExchange& exchange_;
    const OperatorInventory& operators_;
    ModelArchive& models_;
    ContentArchive& content_;
    EventPublisher& events_;
    std::mutex mutex_;  // 两次导入不要交错
};

// 领域事件：方案包已导入
struct RecipePackageImported final : DomainEvent {
    std::string packageId, recipeVersionId;
    RecipePackageImported(std::string p, std::string r) : packageId(std::move(p)), recipeVersionId(std::move(r)) {}
    const char* name() const override { return "RecipePackageImported"; }
};

}  // namespace vision::release
