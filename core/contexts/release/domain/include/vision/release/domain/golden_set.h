#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::release {

using GoldenSetId = std::string;

// 人确认的真实判定。只有“合格 / 不合格”两种：ERROR 不是一种真实情况，而是检测出了问题。
enum class Truth { Ok, Ng };

// 值对象：基准图集里的一个用例
struct GoldenCase {
    std::string id;
    std::string imageRef;  // 图像的位置
    Truth truth = Truth::Ok;
    // 这幅图像是哪个样本（BC-9 数据里按图像内容识别的 ID）。有了它，才能检查规则 N1：
    // 留作基准的样本不能用于训练。没有接上数据上下文的程序里它是空的。
    std::string sampleId;
};

// 值对象：验收标准，由用户设定（例如“漏检 0 张、误报率不超过 2%”）
struct AcceptanceCriteria {
    int maxMisses = 0;                // 最多允许漏检几张
    double maxFalseAlarmRate = 0.0;   // 误报率上限：误报数 ÷ 真实合格的用例数
    int maxErrors = 0;                // 最多允许几张出现 ERROR
};

// 聚合根：基准图集（docs/ddd/05-tactical-design.md §4.2）。带有真实判定的一组图像，加上验收标准。
//
// 这里是 POC-11 需要的最小实现。不变量：
//   - 用例的 ID 不重复
//   - 冻结时至少有一个用例
//   - 冻结后不可修改（修改标准或用例 = 新建一个基准图集）
//   - 同一个样本（同样内容的图像）不能在一个图集里出现两次
// 规则 N1（留作基准的样本不用于训练）涉及数据上下文的样本，由应用层在冻结时通过端口检查。
// 还没有实现：一致性标准。
class GoldenSet : public EventRecorder {
public:
    static Result<GoldenSet> create(GoldenSetId id, std::string name);
    static GoldenSet restore(GoldenSetId id, std::string name, std::vector<GoldenCase> cases, AcceptanceCriteria criteria,
                             bool frozen);

    Status addCase(GoldenCase goldenCase);
    Status setCriteria(AcceptanceCriteria criteria);
    Status freeze();

    const GoldenSetId& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::vector<GoldenCase>& cases() const { return cases_; }
    const AcceptanceCriteria& criteria() const { return criteria_; }
    bool frozen() const { return frozen_; }
    const GoldenCase* findCase(const std::string& caseId) const;

private:
    GoldenSet() = default;
    GoldenSetId id_;
    std::string name_;
    std::vector<GoldenCase> cases_;
    AcceptanceCriteria criteria_;
    bool frozen_ = false;
};

struct GoldenSetFrozen final : DomainEvent {
    std::string goldenSetId;
    int caseCount;
    GoldenSetFrozen(std::string id, int n) : goldenSetId(std::move(id)), caseCount(n) {}
    const char* name() const override { return "GoldenSetFrozen"; }
};

class GoldenSetRepository {
public:
    virtual ~GoldenSetRepository() = default;
    virtual Status save(const GoldenSet& set) = 0;
    virtual std::optional<GoldenSet> find(const GoldenSetId& id) = 0;
    virtual std::vector<GoldenSetId> listIds() = 0;
};

// ===================== 评估规则 =====================
// “对照真实判定，算出误报和漏检”的规则只有这一份。验收检查、对比评估、方案助手的实验都用它
// （docs/ddd/08-platforms-and-training.md §4）。

// 检测给出的结果
enum class Observed { Ok, Ng, Error };

// 一个用例上的检测结果
struct CaseResult {
    std::string caseId;
    Observed observed = Observed::Ok;
    double durationMs = 0;
};

// 值对象：一次评估
struct CaseEvaluation {
    int total = 0;
    int truthOk = 0;       // 真实合格的用例数
    int truthNg = 0;       // 真实不合格的用例数
    int misses = 0;        // 漏检：真实不合格，却判成了合格
    int falseAlarms = 0;   // 误报：真实合格，却判成了不合格
    int errors = 0;        // 检测出现 ERROR
    double meanMs = 0;
    double maxMs = 0;
    std::vector<std::string> wrongCaseIds;  // 判错或 ERROR 的用例

    double falseAlarmRate() const { return truthOk == 0 ? 0.0 : static_cast<double>(falseAlarms) / truthOk; }
    double missRate() const { return truthNg == 0 ? 0.0 : static_cast<double>(misses) / truthNg; }
};

// 对照真实判定评估一组检测结果。results 可以只覆盖图集的一部分；
// 出现图集里没有的用例、或者同一个用例出现两次，返回错误。
Result<CaseEvaluation> evaluateCases(const GoldenSet& set, const std::vector<CaseResult>& results);

// 是否达到验收标准
bool meets(const CaseEvaluation& evaluation, const AcceptanceCriteria& criteria);

// a 是否比 b 好。依次比较：ERROR 少的好；漏检少的好；误报少的好。完全相同则不算更好。
// 漏检排在误报前面：把不合格品放出去，比把合格品拦下来严重。
bool betterThan(const CaseEvaluation& a, const CaseEvaluation& b);

}  // namespace vision::release
