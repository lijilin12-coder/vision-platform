#include <chrono>
#include <mutex>

#include "vision/common/id.h"
#include "vision/common/log.h"
#include "vision/execution/api.h"
#include "vision/execution/domain/executor.h"
#include "vision/execution/domain/plan.h"

namespace vision::execution {

namespace {

// 适配器：把算子目录里的 Operator 适配成本上下文的 NodeRunner 端口。
class CatalogNodeRunner final : public NodeRunner {
public:
    CatalogNodeRunner(std::unique_ptr<catalog::Operator> op, OperatorSignature sig)
        : op_(std::move(op)), sig_(std::move(sig)) {}

    Result<PortValues> run(const PortValues& inputs, const ParamMap& params, const std::optional<Roi>& roi) override {
        catalog::OpContext ctx(inputs, params, roi);
        try {
            if (auto st = op_->execute(ctx); !st) return st.error();
        } catch (const std::exception& e) {
            return Result<PortValues>::failure("execution.operator_exception", std::string("算子异常：") + e.what());
        }
        for (const auto& out : sig_.outputs) {
            if (ctx.outputs().count(out.name) == 0) {
                return Result<PortValues>::failure("execution.missing_output", "算子没有产出端口 " + out.name);
            }
        }
        return std::move(ctx.outputs());
    }

private:
    std::unique_ptr<catalog::Operator> op_;
    OperatorSignature sig_;
};

class CatalogRunnerFactory final : public NodeRunnerFactory {
public:
    explicit CatalogRunnerFactory(const catalog::OperatorCatalog& c) : catalog_(c) {}
    Result<std::shared_ptr<NodeRunner>> create(const OperatorSignature& sig) override {
        auto op = catalog_.create(sig.id, sig.majorVersion);
        if (!op) return op.error();
        return std::shared_ptr<NodeRunner>(std::make_shared<CatalogNodeRunner>(std::move(op).value(), sig));
    }

private:
    const catalog::OperatorCatalog& catalog_;
};

// 发布语言 → 本上下文的模型（防止上游模型渗透进来）
Result<PlanSpec> toPlanSpec(const authoring::RecipeVersionDocument& doc, const catalog::OperatorCatalog& catalog) {
    if (doc.schemaVersion != authoring::kRecipeDocumentSchemaVersion) {
        return Result<PlanSpec>::failure("execution.schema", "不支持的方案文档版本：" + doc.schemaVersion);
    }
    PlanSpec spec;
    spec.recipeVersionId = doc.versionId;
    for (const auto& n : doc.nodes) {
        auto meta = catalog.find(n.operatorId);
        if (!meta) return Result<PlanSpec>::failure("execution.operator_missing", "本平台缺少算子 " + n.operatorId);
        if (meta->signature.majorVersion != n.operatorMajor) {
            return Result<PlanSpec>::failure("execution.operator_major",
                                             "算子 " + n.operatorId + " 主版本号不匹配（D5-1）");
        }
        spec.nodes.push_back(PlanNode{n.id, meta->signature, n.params, n.roi});
    }
    for (const auto& e : doc.edges) {
        const auto a = e.from.find('.'), b = e.to.find('.');
        if (a == std::string::npos || b == std::string::npos) {
            return Result<PlanSpec>::failure("execution.bad_edge", "连线格式错误：" + e.from + " → " + e.to);
        }
        spec.edges.push_back(PlanEdge{e.from.substr(0, a), e.from.substr(a + 1), e.to.substr(0, b), e.to.substr(b + 1)});
    }
    return spec;
}

InspectionPurpose toDomain(RunPurpose p) {
    switch (p) {
        case RunPurpose::Production: return InspectionPurpose::Production;
        case RunPurpose::Debug: return InspectionPurpose::Debug;
        case RunPurpose::Check: return InspectionPurpose::Check;
    }
    return InspectionPurpose::Production;
}

}  // namespace

std::string describeValue(const Value& v) {
    struct V {
        std::string operator()(std::monostate) const { return "（空）"; }
        std::string operator()(const Image& i) const {
            return "image " + std::to_string(i.width()) + "x" + std::to_string(i.height());
        }
        std::string operator()(const Region& r) const {
            return "region " + std::to_string(r.mask().width()) + "x" + std::to_string(r.mask().height());
        }
        std::string operator()(const BlobList& b) const { return "blobs " + std::to_string(b.size()); }
        std::string operator()(double d) const { return "number " + std::to_string(d); }
        std::string operator()(bool b) const { return b ? "true" : "false"; }
        std::string operator()(const std::string& s) const { return "text " + s; }
        std::string operator()(const Judgement& j) const { return std::string(j.pass ? "合格：" : "不合格：") + j.reason; }
    };
    return std::visit(V{}, v);
}

struct ExecutionService::Impl {
    const catalog::OperatorCatalog& catalog;
    EventPublisher& events;
    CatalogRunnerFactory factory;
    ExecutionPlanner planner;
    Executor executor;
    std::mutex cacheMutex;
    std::map<std::string, std::shared_ptr<const ExecutionPlan>> planCache;  // 按方案版本缓存（02-architecture §6.4）

    Impl(const catalog::OperatorCatalog& c, EventPublisher& e) : catalog(c), events(e), factory(c), planner(factory) {}

    Result<std::shared_ptr<const ExecutionPlan>> planFor(const authoring::RecipeVersionDocument& doc, bool useCache) {
        using R = Result<std::shared_ptr<const ExecutionPlan>>;
        if (useCache) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (auto it = planCache.find(doc.versionId); it != planCache.end()) return it->second;
        }
        auto spec = toPlanSpec(doc, catalog);
        if (!spec) return spec.error();
        auto plan = planner.plan(spec.value());
        if (!plan) return plan.error();
        auto shared = std::make_shared<const ExecutionPlan>(std::move(plan).value());
        if (useCache) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            planCache[doc.versionId] = shared;
        }
        return R(shared);
    }

    Result<DebugReport> run(const authoring::RecipeVersionDocument& doc, const Image& frame, RunPurpose purpose) {
        const auto t0 = std::chrono::steady_clock::now();
        // 调试运行的对象可能是草稿，内容随时会变，所以不缓存
        auto plan = planFor(doc, purpose != RunPurpose::Debug);
        if (!plan) return plan.error();

        Inspection inspection =
            Inspection::start(newUuid(), toDomain(purpose), doc.versionId, plan.value()->nodeOrder());
        auto outputs = executor.run(*plan.value(), frame, inspection);
        if (!outputs) return outputs.error();
        events.publishAll(inspection.pullEvents());

        DebugReport dr;
        auto& r = dr.report;
        r.inspectionId = inspection.id();
        r.recipeVersionId = inspection.recipeVersionId();
        r.purpose = toString(inspection.purpose());
        r.verdict = toString(inspection.verdict().kind);
        for (const auto& reason : inspection.verdict().reasons) r.reasons.push_back({reason.nodeId, reason.pass, reason.text});
        const auto& steps = plan.value()->steps();
        for (size_t i = 0; i < steps.size(); ++i) {
            const auto& e = inspection.nodeExecutions()[i];
            NodeReport nr{e.nodeId, steps[i].node.signature.id, toString(e.status), e.durationMs, e.message, {}};
            for (const auto& [port, value] : outputs.value()[e.nodeId]) nr.outputs[port] = describeValue(value);
            r.nodes.push_back(std::move(nr));
        }
        r.totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        // 日志由应用层来写，领域层不写（它不依赖任何技术）。
        // 节点失败是需要排查的情况，记为 warn；每次检测的结果量很大，记为 debug，平时不输出。
        for (const auto& n : r.nodes) {
            if (n.status == "failed") {
                log::warn("execution", "节点执行失败",
                          {{"inspection", r.inspectionId}, {"node", n.nodeId}, {"operator", n.operatorId}, {"reason", n.message}});
            }
        }
        log::debug("execution", "检测完成",
                   {{"inspection", r.inspectionId}, {"recipe", r.recipeVersionId}, {"purpose", r.purpose},
                    {"verdict", r.verdict}, {"ms", std::to_string(r.totalMs)}});
        dr.outputs = std::move(outputs).value();
        return dr;
    }
};

ExecutionService::ExecutionService(const catalog::OperatorCatalog& catalog, EventPublisher& events)
    : impl_(std::make_unique<Impl>(catalog, events)) {}

ExecutionService::~ExecutionService() = default;

Result<InspectionReport> ExecutionService::inspect(const authoring::RecipeVersionDocument& recipe, const Image& frame,
                                                   RunPurpose purpose) {
    auto r = impl_->run(recipe, frame, purpose);
    if (!r) return r.error();
    return std::move(r).value().report;
}

Status ExecutionService::prepare(const authoring::RecipeVersionDocument& recipe) {
    auto plan = impl_->planFor(recipe, true);
    if (!plan) return plan.error();
    return Status::success();
}

Result<DebugReport> ExecutionService::debugRun(const authoring::RecipeVersionDocument& recipe, const Image& frame) {
    return impl_->run(recipe, frame, RunPurpose::Debug);
}

}  // namespace vision::execution
