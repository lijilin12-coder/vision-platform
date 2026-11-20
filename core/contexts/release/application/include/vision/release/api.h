#pragma once

// BC-4 上线管理的公开接口（POC-9 需要的最小部分）。

#include <map>
#include <mutex>
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

// 应用服务。可以从多个线程调用（工位在后台线程里加载上线包）。
class ReleaseService {
public:
    ReleaseService(ReleaseRepository& releases, const RecipeVersionSource& recipes, EventPublisher& events);

    // 用一个已发布的方案版本创建上线包，返回上线包 ID。方案版本不存在时被拒绝。
    Result<std::string> createRelease(const std::string& recipeVersionId);
    Status withdraw(const std::string& releaseId);

    Result<ReleaseManifest> getManifest(const std::string& releaseId) const;
    Result<ReleaseContent> getContent(const std::string& releaseId) const;
    std::vector<std::string> listReleases() const;

private:
    ReleaseRepository& releases_;
    const RecipeVersionSource& recipes_;
    EventPublisher& events_;
    mutable std::mutex mutex_;  // 保护仓储：仓储的实现不要求线程安全
};

// ===================== 基准图集与评估 =====================

struct GoldenCaseView {
    std::string id;
    std::string imageRef;
    bool truthNg = false;  // 人确认的真实判定：是否不合格
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
    Result<std::string> createGoldenSet(const GoldenSetDefinition& definition);
    Result<GoldenSetView> getGoldenSet(const std::string& goldenSetId) const;
    std::vector<std::string> listGoldenSets() const;

    // 对照真实判定评估一组检测结果。results 可以只覆盖图集的一部分。
    Result<EvaluationView> evaluate(const std::string& goldenSetId, const std::vector<CaseResultView>& results) const;

    // a 是否比 b 好：ERROR 少的好，其次漏检少的好，再其次误报少的好。
    static bool betterThan(const EvaluationView& a, const EvaluationView& b);

private:
    GoldenSetRepository& sets_;
    EventPublisher& events_;
    mutable std::mutex mutex_;
};

}  // namespace vision::release
