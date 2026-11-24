#include <sstream>
#include <iomanip>
#include <functional>
#include <array>
#include <algorithm>
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
    CatalogNodeRunner(std::unique_ptr<catalog::Operator> op, OperatorSignature sig,
                      std::map<std::string, MatchTemplateData> templates,
                      std::map<std::string, std::shared_ptr<const InferenceModel>> models)
        : op_(std::move(op)), sig_(std::move(sig)), templates_(std::move(templates)), models_(std::move(models)) {}

    Result<NodeOutput> run(const PortValues& inputs, const ParamMap& params, const std::optional<Roi>& roi) override {
        catalog::OpContext ctx(inputs, params, roi, &templates_, &models_);
        try {
            if (auto st = op_->execute(ctx); !st) return st.error();
        } catch (const std::exception& e) {
            return Result<NodeOutput>::failure("execution.operator_exception", std::string("算子异常：") + e.what());
        }
        for (const auto& out : sig_.outputs) {
            if (ctx.outputs().count(out.name) == 0) {
                return Result<NodeOutput>::failure("execution.missing_output", "算子没有产出端口 " + out.name);
            }
        }
        return NodeOutput{std::move(ctx.outputs()), std::move(ctx.resultItems())};
    }

private:
    std::unique_ptr<catalog::Operator> op_;
    OperatorSignature sig_;
    std::map<std::string, MatchTemplateData> templates_;
    std::map<std::string, std::shared_ptr<const InferenceModel>> models_;
};

class CatalogRunnerFactory final : public NodeRunnerFactory {
public:
    explicit CatalogRunnerFactory(const catalog::OperatorCatalog& c) : catalog_(c) {}
    Result<std::shared_ptr<NodeRunner>> create(const PlanNode& node) override {
        const OperatorSignature& sig = node.signature;
        auto op = catalog_.create(sig.id, sig.majorVersion);
        if (!op) return op.error();
        return std::shared_ptr<NodeRunner>(std::make_shared<CatalogNodeRunner>(std::move(op).value(), sig, node.templates, node.models));
    }

private:
    const catalog::OperatorCatalog& catalog_;
};

// 发布语言 → 本上下文的模型（防止上游模型渗透进来）
// allowUnsetTemplates：调试运行时为真。模板参数还没有设置的节点照样编译进计划，运行到它时才失败；
// 它上游的节点正常运行、中间结果照样能看。人和方案助手都靠这个从“预处理之后的图像”上框模板。
Result<PlanSpec> toPlanSpec(const authoring::RecipeVersionDocument& doc, const catalog::OperatorCatalog& catalog,
                            TemplateSource* templateSource, ModelSource* modelSource, bool allowUnsetTemplates) {
    if (!authoring::isSupportedRecipeDocumentSchema(doc.schemaVersion)) {
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
        PlanNode node{n.id, meta->signature, n.params, n.roi, {}, {}};
        // 匹配模板：现在就取出来、解码好。出了问题在这里（加载方案时）暴露，而不是等到产线上第一次检测
        for (const auto& def : meta->signature.params) {
            if (def.type != ParamType::Template) continue;
            const auto value = n.params.find(def.name);
            const std::string templateId =
                value != n.params.end() && std::holds_alternative<std::string>(value->second) ? std::get<std::string>(value->second) : "";
            const std::string where = "节点 " + n.id + " 的模板参数 " + def.name;
            if (templateId.empty()) {
                if (allowUnsetTemplates) continue;
                return Result<PlanSpec>::failure("execution.template_unset", where + " 还没有设置");
            }
            const auto ref = std::find_if(doc.templates.begin(), doc.templates.end(),
                                          [&](const authoring::RecipeDocumentTemplate& t) { return t.id == templateId; });
            if (ref == doc.templates.end()) {
                return Result<PlanSpec>::failure("execution.template_missing", where + " 指向的匹配模板 " + templateId + " 不在方案里");
            }
            if (templateSource == nullptr) {
                return Result<PlanSpec>::failure("execution.no_template_source", "这个程序没有配置读取匹配模板的地方");
            }
            auto image = templateSource->load(*ref);
            if (!image) {
                return Result<PlanSpec>::failure(image.error().code, "匹配模板“" + ref->name + "”读取失败：" + image.error().message);
            }
            if (image.value().width() != ref->width || image.value().height() != ref->height) {
                return Result<PlanSpec>::failure("execution.template_corrupt", "匹配模板“" + ref->name + "”的尺寸和方案里记录的不一致");
            }
            node.templates[def.name] = MatchTemplateData{std::move(image).value(), ref->referenceX, ref->referenceY,
                                                         ref->sourceWidth, ref->sourceHeight};
        }
        // 模型：同样现在就加载好。调试运行（allowUnsetTemplates）时：没有设置的先跳过，未发布的模型也可以试
        for (const auto& def : meta->signature.params) {
            if (def.type != ParamType::Model) continue;
            const auto value = n.params.find(def.name);
            const std::string modelId =
                value != n.params.end() && std::holds_alternative<std::string>(value->second) ? std::get<std::string>(value->second) : "";
            const std::string where = "节点 " + n.id + " 的模型参数 " + def.name;
            if (modelId.empty()) {
                if (allowUnsetTemplates) continue;
                return Result<PlanSpec>::failure("execution.model_unset", where + " 还没有设置");
            }
            if (modelSource == nullptr) {
                return Result<PlanSpec>::failure("execution.no_model_source", "这个程序没有配置推理引擎，不能运行用到模型的方案");
            }
            auto model = modelSource->load(modelId, /*requirePublished=*/!allowUnsetTemplates);
            if (!model) return Result<PlanSpec>::failure(model.error().code, where + "：" + model.error().message);
            node.models[def.name] = std::move(model).value();
        }
        spec.nodes.push_back(std::move(node));
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
        std::string operator()(const Pose& p) const {
            std::ostringstream os;
            os << std::fixed << std::setprecision(2) << "pose (" << p.x << ", " << p.y << ") 旋转 " << p.angle << "°";
            return os.str();
        }
    };
    return std::visit(V{}, v);
}

namespace {

// 灰度直方图。彩色图像取三个通道的平均值。
std::array<std::size_t, 256> histogramOf(const Image& image) {
    std::array<std::size_t, 256> h{};
    const int channels = image.channels();
    const std::uint8_t* p = image.data();
    const std::size_t pixels = static_cast<std::size_t>(image.width()) * image.height();
    for (std::size_t i = 0; i < pixels; ++i, p += channels) {
        const int gray = channels == 1 ? p[0] : (p[0] + p[1] + p[2]) / 3;
        ++h[static_cast<std::size_t>(gray)];
    }
    return h;
}

}  // namespace

std::string describeStatistics(const Value& v) {
    struct V {
        std::string operator()(const Image& image) const {
            if (image.empty()) return {};
            const auto h = histogramOf(image);
            const std::size_t total = static_cast<std::size_t>(image.width()) * image.height();
            // 分位数：例如 p99 = 180 表示 99% 的像素不比 180 亮
            const auto percentile = [&](double q) {
                const auto target = static_cast<std::size_t>(q * static_cast<double>(total - 1));
                std::size_t seen = 0;
                for (int g = 0; g < 256; ++g) {
                    seen += h[static_cast<std::size_t>(g)];
                    if (seen > target) return g;
                }
                return 255;
            };
            std::ostringstream os;
            os << "灰度 最小 " << percentile(0) << "，p1 " << percentile(0.01) << "，p50 " << percentile(0.5) << "，p90 "
               << percentile(0.9) << "，p99 " << percentile(0.99) << "，p99.9 " << percentile(0.999) << "，最大 "
               << percentile(1.0);
            return os.str();
        }
        std::string operator()(const Region& r) const {
            const Image& mask = r.mask();
            if (mask.empty()) return {};
            const std::size_t total = static_cast<std::size_t>(mask.width()) * mask.height();
            std::size_t selected = 0;
            for (std::size_t i = 0; i < total; ++i) selected += mask.data()[i] != 0;
            std::ostringstream os;
            os << "选中 " << selected << " 个像素（" << std::fixed << std::setprecision(2)
               << 100.0 * static_cast<double>(selected) / static_cast<double>(total) << "%）";
            return os.str();
        }
        std::string operator()(const BlobList& blobs) const {
            if (blobs.empty()) return {};
            std::vector<double> areas;
            for (const auto& b : blobs) areas.push_back(b.area);
            std::sort(areas.begin(), areas.end(), std::greater<>());
            const std::size_t shown = std::min<std::size_t>(areas.size(), 10);  // 最多列 10 个，其余只给个数
            std::ostringstream os;
            os << "面积从大到小";
            for (std::size_t i = 0; i < shown; ++i) os << " " << areas[i];
            if (areas.size() > shown) os << " ……（还有 " << areas.size() - shown << " 个更小的，最小 " << areas.back() << "）";
            return os.str();
        }
        std::string operator()(std::monostate) const { return {}; }
        std::string operator()(double) const { return {}; }
        std::string operator()(bool) const { return {}; }
        std::string operator()(const std::string&) const { return {}; }
        std::string operator()(const Judgement&) const { return {}; }
        std::string operator()(const Pose& p) const {
            std::ostringstream os;
            os << std::fixed << std::setprecision(2) << "相对基准位置平移 (" << p.x - p.referenceX << ", " << p.y - p.referenceY << ")";
            return os.str();
        }
    };
    return std::visit(V{}, v);
}

struct ExecutionService::Impl {
    const catalog::OperatorCatalog& catalog;
    EventPublisher& events;
    CatalogRunnerFactory factory;
    ExecutionPlanner planner;
    Executor executor;
    TemplateSource* templateSource = nullptr;
    ModelSource* modelSource = nullptr;
    std::mutex cacheMutex;
    std::map<std::string, std::shared_ptr<const ExecutionPlan>> planCache;  // 按方案版本缓存（02-architecture §6.4）

    Impl(const catalog::OperatorCatalog& c, EventPublisher& e) : catalog(c), events(e), factory(c), planner(factory) {}

    Result<std::shared_ptr<const ExecutionPlan>> planFor(const authoring::RecipeVersionDocument& doc, bool useCache) {
        using R = Result<std::shared_ptr<const ExecutionPlan>>;
        if (useCache) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (auto it = planCache.find(doc.versionId); it != planCache.end()) return it->second;
        }
        // 调试运行不用缓存（useCache 为假），也只有它允许模板还没有设置
        auto spec = toPlanSpec(doc, catalog, templateSource, modelSource, /*allowUnsetTemplates=*/!useCache);
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
        r.result = inspection.result();
        const auto& steps = plan.value()->steps();
        for (size_t i = 0; i < steps.size(); ++i) {
            const auto& e = inspection.nodeExecutions()[i];
            NodeReport nr{e.nodeId, steps[i].node.signature.id, toString(e.status), e.durationMs, e.message, {}, {}};
            for (const auto& [port, value] : outputs.value()[e.nodeId]) {
                nr.outputs[port] = describeValue(value);
                if (purpose == RunPurpose::Debug) {
                    if (auto stats = describeStatistics(value); !stats.empty()) nr.statistics[port] = std::move(stats);
                }
            }
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

void ExecutionService::useTemplateSource(TemplateSource& source) { impl_->templateSource = &source; }
void ExecutionService::useModelSource(ModelSource& source) { impl_->modelSource = &source; }

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
