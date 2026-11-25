#include "http_api.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "station_runner.h"
#include "vision/common/id.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::runtime {

using nlohmann::json;
using httplib::Request;
using httplib::Response;

namespace {

constexpr const char* kJson = "application/json; charset=utf-8";
constexpr std::size_t kMaxUploadBytes = 64u * 1024 * 1024;  // 一幅图像最大 64 MB
constexpr std::size_t kKeptDebugRuns = 8;                    // 只留最近几次调试运行的结果

// ---------- 领域里的值 → JSON ----------

json toJson(const ParamValue& v) {
    return std::visit([](const auto& x) { return json(x); }, v);
}

json toJson(const std::optional<Roi>& roi) {
    if (!roi) return nullptr;
    return {{"x", roi->x()}, {"y", roi->y()}, {"width", roi->width()}, {"height", roi->height()}};
}

const char* paramTypeName(ParamType t) {
    switch (t) {
        case ParamType::Number: return "number";
        case ParamType::Bool: return "bool";
        case ParamType::Text: return "text";
        case ParamType::Choice: return "choice";
        case ParamType::Template: return "template";
        case ParamType::Model: return "model";
    }
    return "number";
}

json portsJson(const std::vector<PortDef>& ports) {
    json a = json::array();
    for (const auto& p : ports) a.push_back({{"name", p.name}, {"type", toString(p.type)}, {"optional", p.optional}});
    return a;
}

json operatorJson(const catalog::OperatorMeta& m) {
    json params = json::array();
    for (const auto& p : m.signature.params) {
        json j = {{"name", p.name}, {"type", paramTypeName(p.type)}, {"default", toJson(p.defaultValue)}, {"label", p.label}};
        if (p.min) j["min"] = *p.min;
        if (p.max) j["max"] = *p.max;
        if (p.type == ParamType::Choice) j["choices"] = p.choices;
        params.push_back(std::move(j));
    }
    return {{"id", m.signature.id},
            {"majorVersion", m.signature.majorVersion},
            {"displayName", m.displayName},
            {"category", m.category},
            {"description", m.description},
            {"inputs", portsJson(m.signature.inputs)},
            {"outputs", portsJson(m.signature.outputs)},
            {"params", std::move(params)},
            {"contributesToVerdict", m.signature.contributesToVerdict()}};
}

json recipeJson(const std::string& recipeId, const authoring::RecipeVersionDocument& doc) {
    json nodes = json::array(), edges = json::array(), templates = json::array();
    for (const auto& n : doc.nodes) {
        json params = json::object();
        for (const auto& [name, value] : n.params) params[name] = toJson(value);
        nodes.push_back({{"id", n.id}, {"operatorId", n.operatorId}, {"operatorMajor", n.operatorMajor}, {"params", std::move(params)},
                         {"roi", toJson(n.roi)}});
    }
    for (const auto& e : doc.edges) edges.push_back({{"from", e.from}, {"to", e.to}});
    for (const auto& t : doc.templates) {
        templates.push_back({{"id", t.id}, {"name", t.name}, {"width", t.width}, {"height", t.height},
                             {"referenceX", t.referenceX}, {"referenceY", t.referenceY},
                             {"sourceWidth", t.sourceWidth}, {"sourceHeight", t.sourceHeight}});
    }
    return {{"id", recipeId}, {"name", doc.recipeName}, {"nodes", std::move(nodes)}, {"edges", std::move(edges)},
            {"templates", std::move(templates)}};
}

// 调试运行里一个输出的说明。图像和区域不放进 JSON：给一个标记，界面再按地址去取 PNG
json outputJson(const Value& value) {
    struct Visitor {
        json operator()(std::monostate) const { return {{"type", "none"}}; }
        json operator()(const Image& i) const {
            return {{"type", "image"}, {"hasImage", true}, {"width", i.width()}, {"height", i.height()}, {"channels", i.channels()}};
        }
        json operator()(const Region& r) const {
            return {{"type", "region"}, {"hasImage", true}, {"width", r.mask().width()}, {"height", r.mask().height()}};
        }
        json operator()(const BlobList& blobs) const {
            json list = json::array();
            for (const auto& b : blobs) {
                list.push_back({{"area", b.area}, {"centerX", b.centerX}, {"centerY", b.centerY},
                                {"left", b.left}, {"top", b.top}, {"width", b.width}, {"height", b.height}});
            }
            return {{"type", "blobs"}, {"value", std::move(list)}};
        }
        json operator()(double d) const { return {{"type", "number"}, {"value", d}}; }
        json operator()(bool b) const { return {{"type", "bool"}, {"value", b}}; }
        json operator()(const std::string& s) const { return {{"type", "text"}, {"value", s}}; }
        json operator()(const Judgement& j) const { return {{"type", "judgement"}, {"value", {{"pass", j.pass}, {"reason", j.reason}}}}; }
        json operator()(const Pose& p) const {
            return {{"type", "pose"},
                    {"value", {{"x", p.x}, {"y", p.y}, {"angle", p.angle}, {"referenceX", p.referenceX}, {"referenceY", p.referenceY}}}};
        }
    };
    return std::visit(Visitor{}, value);
}

json acceptanceJson(const release::AcceptanceView& a) {
    json wrong = json::array();
    for (const auto& w : a.wrongCases) {
        wrong.push_back({{"caseId", w.caseId}, {"imageRef", w.imageRef}, {"truth", w.truthNg ? "NG" : "OK"}, {"observed", w.observed},
                         {"kind", w.kind}});
    }
    return {{"releaseId", a.releaseId}, {"goldenSetId", a.goldenSetId}, {"goldenSetName", a.goldenSetName},
            {"checkedAtMs", a.checkedAtMs}, {"passed", a.passed},
            {"criteria", {{"maxMisses", a.maxMisses}, {"maxFalseAlarmRate", a.maxFalseAlarmRate}, {"maxErrors", a.maxErrors}}},
            {"total", a.total}, {"truthOk", a.truthOk}, {"truthNg", a.truthNg}, {"misses", a.misses}, {"falseAlarms", a.falseAlarms},
            {"errors", a.errors}, {"falseAlarmRate", a.falseAlarmRate}, {"meanMs", a.meanMs}, {"maxMs", a.maxMs},
            {"wrongCases", std::move(wrong)}};
}

json resultJson(const ResultItems& result) {
    json defects = json::array(), measurements = json::array(), poses = json::array();
    for (const auto& d : result.defects) {
        defects.push_back({{"nodeId", d.nodeId}, {"category", d.category}, {"centerX", d.centerX}, {"centerY", d.centerY},
                           {"left", d.left}, {"top", d.top}, {"width", d.width}, {"height", d.height}, {"area", d.area}});
    }
    for (const auto& m : result.measurements) {
        json j = {{"nodeId", m.nodeId}, {"name", m.name}, {"value", m.value}, {"unit", m.unit}, {"pass", m.pass}};
        if (m.lower) j["lower"] = *m.lower;
        if (m.upper) j["upper"] = *m.upper;
        measurements.push_back(std::move(j));
    }
    for (const auto& p : result.poses) {
        poses.push_back({{"nodeId", p.nodeId}, {"found", p.found}, {"score", p.score}, {"x", p.pose.x}, {"y", p.pose.y},
                         {"angle", p.pose.angle}, {"referenceX", p.pose.referenceX}, {"referenceY", p.pose.referenceY}});
    }
    return {{"defects", std::move(defects)}, {"measurements", std::move(measurements)}, {"poses", std::move(poses)}};
}

// ---------- 回复 ----------

void reply(Response& res, int status, const json& body) {
    res.status = status;
    // 第三个参数：遇到不是合法 UTF-8 的文字时换成替代符号，而不是抛异常（文字可能来自文件名）
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), kJson);
}

// 应用服务报告的失败 → HTTP。code 和 message 原样带出去。
//   找不到（code 里有 not_found）→ 404；其余都是“请求看得懂，但规则不允许”→ 422
void replyError(Response& res, const Error& e) {
    const int status = e.code.find("not_found") != std::string::npos ? 404 : 422;
    reply(res, status, {{"code", e.code}, {"message", e.message}});
}

void replyBadRequest(Response& res, const std::string& message) {
    reply(res, 400, {{"code", "api.bad_request"}, {"message", message}});
}

// 请求体 → JSON 对象。不是对象就回复 400 并返回空
std::optional<json> bodyObject(const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (!body.is_object()) {
        replyBadRequest(res, "请求体必须是一个 JSON 对象");
        return std::nullopt;
    }
    return body;
}

// 取一个必填的文本字段；没有就回复 400
std::optional<std::string> textField(const json& body, const char* name, Response& res) {
    if (!body.contains(name) || !body[name].is_string()) {
        replyBadRequest(res, std::string("缺少文本字段 ") + name);
        return std::nullopt;
    }
    return body[name].get<std::string>();
}

std::optional<double> numberField(const json& body, const char* name, Response& res) {
    if (!body.contains(name) || !body[name].is_number()) {
        replyBadRequest(res, std::string("缺少数字字段 ") + name);
        return std::nullopt;
    }
    return body[name].get<double>();
}

}  // namespace

struct ApiServer::Impl {
    explicit Impl(platform::Platform& p) : platform(p), runner(p) {}

    platform::Platform& platform;
    platform::StationRunner runner;  // 让工位持续运行（界面上的“开始 / 停止”）
    httplib::Server server;

    // 调试运行的结果：只是给界面看的临时东西，不是领域里的概念，所以放在这里的内存里，不进仓储
    struct DebugRun {
        std::string id;
        std::map<std::string, std::map<std::string, Value>> outputs;
    };
    std::mutex runsMutex;
    std::deque<DebugRun> runs;

    void routes();
    void operators(const Request&, Response&);
    void debugRun(const Request&, Response&);
    void debugOutput(const Request&, Response&);
    void uploadImage(const Request&, Response&);

    // 把方案草稿写成回复（几乎每个编辑操作成功之后都回复最新的草稿，界面不用再问一次）
    void replyRecipe(Response& res, const std::string& recipeId, int status = 200) {
        auto doc = platform.authoring.getDraftDocument(recipeId);
        if (!doc) return replyError(res, doc.error());
        reply(res, status, recipeJson(recipeId, doc.value()));
    }
    void replyStatus(Response& res, const std::string& recipeId, const Status& st) {
        if (!st) return replyError(res, st.error());
        replyRecipe(res, recipeId);
    }
};

void ApiServer::Impl::operators(const Request&, Response& res) {
    json list = json::array();
    for (const auto& m : catalog::OperatorCatalog::global().list()) list.push_back(operatorJson(m));
    reply(res, 200, {{"operators", std::move(list)}});
}

void ApiServer::Impl::uploadImage(const Request& req, Response& res) {
    const std::vector<unsigned char> bytes(req.body.begin(), req.body.end());
    // 先确认它真的是一幅图像，再存：内容仓库里不放看不懂的东西
    auto image = imaging::decodeImage(bytes);
    if (!image) return reply(res, 422, {{"code", image.error().code}, {"message", image.error().message}});
    auto id = platform.contentStore.put(bytes);
    if (!id) return replyError(res, id.error());
    reply(res, 201, {{"imageId", id.value()}, {"width", image.value().width()}, {"height", image.value().height()},
                     {"channels", image.value().channels()}});
}

void ApiServer::Impl::debugRun(const Request& req, Response& res) {
    const std::string recipeId = req.matches[1];
    const auto body = bodyObject(req, res);
    if (!body) return;
    const auto imageId = textField(*body, "imageId", res);
    if (!imageId) return;

    auto bytes = platform.contentStore.get(*imageId);
    if (!bytes) return reply(res, 404, {{"code", "api.image_not_found"}, {"message", "找不到图像 " + *imageId + "：请先上传"}});
    auto image = imaging::decodeImage(bytes.value());
    if (!image) return replyError(res, image.error());
    auto doc = platform.authoring.getDraftDocument(recipeId);
    if (!doc) return replyError(res, doc.error());
    auto run = platform.execution.debugRun(doc.value(), image.value());
    if (!run) return replyError(res, run.error());

    const auto& report = run.value().report;
    const std::string runId = newUuid();
    json nodes = json::array();
    for (const auto& n : report.nodes) {
        json outputs = json::object();
        if (const auto it = run.value().outputs.find(n.nodeId); it != run.value().outputs.end()) {
            for (const auto& [port, value] : it->second) {
                json o = outputJson(value);
                if (const auto s = n.outputs.find(port); s != n.outputs.end()) o["summary"] = s->second;
                if (const auto s = n.statistics.find(port); s != n.statistics.end()) o["statistics"] = s->second;
                if (o.value("hasImage", false)) o["imageUrl"] = "/api/debug-runs/" + runId + "/outputs/" + n.nodeId + "/" + port;
                outputs[port] = std::move(o);
            }
        }
        nodes.push_back({{"nodeId", n.nodeId}, {"operatorId", n.operatorId}, {"status", n.status}, {"durationMs", n.durationMs},
                         {"message", n.message}, {"outputs", std::move(outputs)}});
    }
    json reasons = json::array();
    for (const auto& r : report.reasons) reasons.push_back({{"nodeId", r.nodeId}, {"pass", r.pass}, {"text", r.text}});
    {
        std::lock_guard<std::mutex> lock(runsMutex);
        runs.push_back(DebugRun{runId, std::move(run.value().outputs)});
        while (runs.size() > kKeptDebugRuns) runs.pop_front();
    }
    reply(res, 201, {{"runId", runId}, {"verdict", report.verdict}, {"totalMs", report.totalMs}, {"reasons", std::move(reasons)},
                     {"result", resultJson(report.result)}, {"nodes", std::move(nodes)}});
}

void ApiServer::Impl::debugOutput(const Request& req, Response& res) {
    const std::string runId = req.matches[1], nodeId = req.matches[2], port = req.matches[3];
    Image image;
    {
        std::lock_guard<std::mutex> lock(runsMutex);
        const auto run = std::find_if(runs.begin(), runs.end(), [&](const DebugRun& r) { return r.id == runId; });
        if (run == runs.end()) {
            return reply(res, 404, {{"code", "api.run_not_found"}, {"message", "这次调试运行的结果已经不在了（只保留最近几次），请重新运行"}});
        }
        const auto notFound = [&] {
            reply(res, 404, {{"code", "api.output_not_found"}, {"message", "节点 " + nodeId + " 没有输出 " + port}});
        };
        const auto node = run->outputs.find(nodeId);
        if (node == run->outputs.end()) return notFound();
        const auto value = node->second.find(port);
        if (value == node->second.end()) return notFound();
        // Image 内部共享像素，复制它很便宜；编码放到锁外面做
        if (const auto* i = std::get_if<Image>(&value->second)) image = *i;
        else if (const auto* r = std::get_if<Region>(&value->second)) image = r->mask();
        else return reply(res, 422, {{"code", "api.not_an_image"}, {"message", "这个输出不是图像，它的值已经在调试运行的结果里了"}});
    }
    auto png = imaging::encodePng(image);
    if (!png) return replyError(res, png.error());
    // 一次调试运行的结果不会再变：让浏览器放心缓存
    res.set_header("Cache-Control", "private, max-age=3600, immutable");
    res.set_content(reinterpret_cast<const char*>(png.value().data()), png.value().size(), "image/png");
}

void ApiServer::Impl::routes() {
    auto& a = platform.authoring;
    const std::string id = "([^/]+)";

    server.Get("/api/health", [](const Request&, Response& res) { reply(res, 200, {{"ok", true}, {"service", "vision-runtime"}}); });
    server.Get("/api/operators", [this](const Request& q, Response& s) { operators(q, s); });

    // ---- 方案 ----
    server.Get("/api/recipes", [this, &a](const Request&, Response& res) {
        json list = json::array();
        for (const auto& recipeId : a.listRecipes()) {
            if (auto draft = a.getDraft(recipeId)) list.push_back({{"id", recipeId}, {"name", draft.value().name}});
        }
        reply(res, 200, {{"recipes", std::move(list)}});
    });
    server.Post("/api/recipes", [this, &a](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto name = textField(*body, "name", res);
        if (!name) return;
        auto recipe = a.createRecipe(*name);
        if (!recipe) return replyError(res, recipe.error());
        replyRecipe(res, recipe.value(), 201);
    });
    server.Get("/api/recipes/" + id, [this](const Request& req, Response& res) { replyRecipe(res, req.matches[1]); });

    server.Post("/api/recipes/" + id + "/nodes", [this, &a](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto operatorId = textField(*body, "operatorId", res);
        if (!operatorId) return;
        auto node = a.addNode(req.matches[1], *operatorId);
        if (!node) return replyError(res, node.error());
        auto doc = a.getDraftDocument(req.matches[1]);
        if (!doc) return replyError(res, doc.error());
        json out = recipeJson(req.matches[1], doc.value());
        out["addedNodeId"] = node.value();
        reply(res, 201, out);
    });
    server.Delete("/api/recipes/" + id + "/nodes/" + id, [this, &a](const Request& req, Response& res) {
        replyStatus(res, req.matches[1], a.removeNode(req.matches[1], req.matches[2]));
    });

    // 连线和删线：请求体 {"from": "n1.image", "to": "n2.image"}
    const auto edge = [this, &a](bool add) {
        return [this, &a, add](const Request& req, Response& res) {
            const auto body = bodyObject(req, res);
            if (!body) return;
            const auto from = textField(*body, "from", res);
            if (!from) return;
            const auto to = textField(*body, "to", res);
            if (!to) return;
            const std::string recipeId = req.matches[1];
            replyStatus(res, recipeId, add ? a.connect(recipeId, *from, *to) : a.disconnect(recipeId, *from, *to));
        };
    };
    server.Post("/api/recipes/" + id + "/edges", edge(true));
    server.Delete("/api/recipes/" + id + "/edges", edge(false));

    // 改参数：请求体 {"value": 128}。值的类型对不对、在不在范围里，由方案编排检查
    server.Put("/api/recipes/" + id + "/nodes/" + id + "/params/" + id, [this, &a](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        if (!body->contains("value")) return replyBadRequest(res, "缺少字段 value");
        const json& v = (*body)["value"];
        ParamValue value;
        if (v.is_boolean()) value = v.get<bool>();
        else if (v.is_number()) value = v.get<double>();
        else if (v.is_string()) value = v.get<std::string>();
        else return replyBadRequest(res, "value 必须是数字、布尔值或文本");
        replyStatus(res, req.matches[1], a.setParam(req.matches[1], req.matches[2], req.matches[3], value));
    });

    server.Put("/api/recipes/" + id + "/nodes/" + id + "/roi", [this, &a](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto x = numberField(*body, "x", res);
        if (!x) return;
        const auto y = numberField(*body, "y", res);
        if (!y) return;
        const auto width = numberField(*body, "width", res);
        if (!width) return;
        const auto height = numberField(*body, "height", res);
        if (!height) return;
        auto roi = Roi::rect(*x, *y, *width, *height);
        if (!roi) return replyError(res, roi.error());
        replyStatus(res, req.matches[1], a.setRoi(req.matches[1], req.matches[2], roi.value()));
    });
    server.Delete("/api/recipes/" + id + "/nodes/" + id + "/roi", [this, &a](const Request& req, Response& res) {
        replyStatus(res, req.matches[1], a.clearRoi(req.matches[1], req.matches[2]));
    });

    server.Post("/api/recipes/" + id + "/publish", [&a](const Request& req, Response& res) {
        auto version = a.publish(req.matches[1]);
        if (!version) return replyError(res, version.error());
        reply(res, 201, {{"versionId", version.value()}});
    });

    // ---- 图像和调试运行 ----
    server.Post("/api/images", [this](const Request& q, Response& s) { uploadImage(q, s); });
    server.Post("/api/recipes/" + id + "/debug-runs", [this](const Request& q, Response& s) { debugRun(q, s); });
    server.Get("/api/debug-runs/" + id + "/outputs/" + id + "/" + id, [this](const Request& q, Response& s) { debugOutput(q, s); });

    // 取回上传过的图像（界面要把原图显示出来）。图像 ID 是内容的哈希，同一个 ID 的内容永远不变
    server.Get("/api/images/" + id, [this](const Request& req, Response& res) {
        auto bytes = platform.contentStore.get(req.matches[1]);
        if (!bytes) return reply(res, 404, {{"code", "api.image_not_found"}, {"message", "找不到图像"}});
        auto image = imaging::decodeImage(bytes.value());
        if (!image) return replyError(res, image.error());
        auto png = imaging::encodePng(image.value());  // 统一转成 PNG：浏览器不一定认 BMP
        if (!png) return replyError(res, png.error());
        res.set_header("Cache-Control", "private, max-age=86400, immutable");
        res.set_content(reinterpret_cast<const char*>(png.value().data()), png.value().size(), "image/png");
    });

    // ---- 上线包 ----
    server.Get("/api/releases", [this](const Request&, Response& res) {
        json list = json::array();
        for (const auto& releaseId : platform.releases.listReleases()) {
            auto m = platform.releases.getManifest(releaseId);
            if (!m) continue;
            // 方案的名称：给人看的。方案版本找不到时（不应该发生）留空
            std::string recipeName;
            if (auto doc = platform.recipeSource.find(m.value().recipeVersionId)) recipeName = doc.value().recipeName;
            json entry = {{"id", releaseId}, {"recipeVersionId", m.value().recipeVersionId}, {"recipeName", recipeName},
                          {"withdrawn", m.value().withdrawn}, {"acceptanceState", m.value().acceptance}, {"acceptance", nullptr}};
            if (auto a = platform.releases.getAcceptance(releaseId)) entry["acceptance"] = acceptanceJson(a.value());
            list.push_back(std::move(entry));
        }
        reply(res, 200, {{"releases", std::move(list)}});
    });
    server.Post("/api/releases", [this](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto version = textField(*body, "recipeVersionId", res);
        if (!version) return;
        auto release = platform.releases.createRelease(*version);
        if (!release) return replyError(res, release.error());
        reply(res, 201, {{"releaseId", release.value()}});
    });

    // 验收检查：在一个基准图集上把上线包跑一遍。请求体 {"goldenSetId": "…"}。
    // 没通过也是 200（passed 为 false）：检查做完了，只是结论是“不行”。用例多的时候这个请求要等一会儿
    server.Post("/api/releases/" + id + "/acceptance", [this](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto goldenSet = textField(*body, "goldenSetId", res);
        if (!goldenSet) return;
        auto a = platform.releases.checkAcceptance(req.matches[1], *goldenSet);
        if (!a) return replyError(res, a.error());
        reply(res, 200, acceptanceJson(a.value()));
    });

    // 导出上线包文件，写到后端那台机器上。请求体 {"path": "某个文件.vrel"}。只有验收通过、没撤回的上线包才能导出
    server.Post("/api/releases/" + id + "/export", [this](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto path = textField(*body, "path", res);
        if (!path) return;
        auto r = platform.packages.exportRelease(req.matches[1], *path);
        if (!r) return replyError(res, r.error());
        reply(res, 200, {{"path", *path}, {"recipeVersionId", r.value().recipeVersionId}, {"templates", r.value().templates},
                         {"models", r.value().models}, {"contentBytes", r.value().contentBytes}});
    });

    // ---- 基准图集 ----
    const auto goldenSetJson = [](const release::GoldenSetView& s) {
        int ng = 0;
        for (const auto& c : s.cases) ng += c.truthNg ? 1 : 0;
        return json{{"id", s.id}, {"name", s.name}, {"cases", s.cases.size()}, {"truthOk", static_cast<int>(s.cases.size()) - ng},
                    {"truthNg", ng},
                    {"criteria", {{"maxMisses", s.criteria.maxMisses}, {"maxFalseAlarmRate", s.criteria.maxFalseAlarmRate},
                                  {"maxErrors", s.criteria.maxErrors}}}};
    };
    server.Get("/api/golden-sets", [this, goldenSetJson](const Request&, Response& res) {
        json list = json::array();
        for (const auto& setId : platform.goldenSets.listGoldenSets()) {
            if (auto s = platform.goldenSets.getGoldenSet(setId)) list.push_back(goldenSetJson(s.value()));
        }
        reply(res, 200, {{"goldenSets", std::move(list)}});
    });
    // 用后端那台机器上的两个目录做一个基准图集。
    // 请求体 {"name", "okDirectory", "ngDirectory", "maxMisses", "maxFalseAlarmRate", "maxErrors"}；目录可以只给一个，标准不给就是 0
    server.Post("/api/golden-sets", [this, goldenSetJson](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto name = textField(*body, "name", res);
        if (!name) return;
        release::CriteriaView criteria;
        std::string ok, ng;
        try {
            ok = body->value("okDirectory", std::string());
            ng = body->value("ngDirectory", std::string());
            criteria.maxMisses = body->value("maxMisses", 0);
            criteria.maxFalseAlarmRate = body->value("maxFalseAlarmRate", 0.0);
            criteria.maxErrors = body->value("maxErrors", 0);
        } catch (const std::exception&) {
            return replyBadRequest(res, "目录必须是文字，验收标准必须是数字");
        }
        auto created = platform::createGoldenSetFromFolders(platform, *name, ok, ng, criteria);
        if (!created) return replyError(res, created.error());
        auto s = platform.goldenSets.getGoldenSet(created.value());
        if (!s) return replyError(res, s.error());
        reply(res, 201, goldenSetJson(s.value()));
    });

    // ---- 工位 ----
    // 工位的当前状态 + 它有没有在持续运行、检测了多少。?after=N：只要序号大于 N 的检测记录
    const auto stationJson = [this](const std::string& stationId, std::uint64_t after) -> Result<json> {
        auto view = platform.stations.getStation(stationId);
        if (!view) return view.error();
        const auto& v = view.value();
        const auto s = runner.status(stationId, after);
        json recent = json::array();
        for (const auto& r : s.recent) {
            json reasons = json::array();
            for (const auto& reason : r.reasons) reasons.push_back({{"nodeId", reason.nodeId}, {"pass", reason.pass}, {"text", reason.text}});
            recent.push_back({{"sequence", r.sequence}, {"frameName", r.frameName}, {"verdict", r.verdict}, {"releaseId", r.releaseId},
                              {"swapped", r.swapped}, {"totalMs", r.totalMs}, {"reasons", std::move(reasons)},
                              {"result", resultJson(r.result)}});
        }
        return json{{"id", v.id}, {"name", v.name}, {"state", v.state}, {"activeRelease", v.activeRelease},
                    {"pendingRelease", v.pendingRelease}, {"pendingReady", v.pendingReady}, {"releaseHistory", v.releaseHistory},
                    {"consecutiveErrors", v.consecutiveErrors}, {"sequence", v.sequence},
                    {"imageSource", {{"kind", v.imageSourceKind}, {"location", v.imageSourceLocation}}},
                    {"runner", {{"running", s.running}, {"idle", s.idle}, {"ok", s.ok}, {"ng", s.ng}, {"error", s.error},
                                {"unreadable", s.unreadable}, {"stoppedReason", s.stoppedReason}}},
                    {"recent", std::move(recent)}};
    };
    const auto replyStation = [stationJson](Response& res, const std::string& stationId, const Status& st = Status::success(),
                                            std::uint64_t after = 0) {
        if (!st) return replyError(res, st.error());
        auto j = stationJson(stationId, after);
        if (!j) return replyError(res, j.error());
        reply(res, 200, j.value());
    };
    server.Get("/api/stations", [this, stationJson](const Request&, Response& res) {
        json list = json::array();
        for (const auto& stationId : platform.stations.listStations()) {
            // 列表里不带检测记录：给一个很大的 after
            if (auto j = stationJson(stationId, ~std::uint64_t{0})) list.push_back(std::move(j).value());
        }
        reply(res, 200, {{"stations", std::move(list)}});
    });
    server.Post("/api/stations", [this, replyStation](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto name = textField(*body, "name", res);
        if (!name) return;
        auto station = platform.stations.createStation(*name);
        if (!station) return replyError(res, station.error());
        replyStation(res, station.value());
    });
    server.Get("/api/stations/" + id, [replyStation](const Request& req, Response& res) {
        std::uint64_t after = 0;
        try {
            if (req.has_param("after")) after = std::stoull(req.get_param_value("after"));
        } catch (const std::exception&) {
            return replyBadRequest(res, "after 必须是一个数字");
        }
        replyStation(res, req.matches[1], Status::success(), after);
    });
    server.Post("/api/stations/" + id + "/deploy", [this, replyStation](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto release = textField(*body, "releaseId", res);
        if (!release) return;
        replyStation(res, req.matches[1], platform.stations.deploy(req.matches[1], *release));
    });
    server.Post("/api/stations/" + id + "/rollback", [this, replyStation](const Request& req, Response& res) {
        replyStation(res, req.matches[1], platform.stations.rollback(req.matches[1]));
    });
    // 图像源：请求体 {"kind": "folder", "location": "某个目录"}。只能在工位停止时换（S7）
    server.Put("/api/stations/" + id + "/image-source", [this, replyStation](const Request& req, Response& res) {
        const auto body = bodyObject(req, res);
        if (!body) return;
        const auto kind = textField(*body, "kind", res);
        if (!kind) return;
        const auto location = textField(*body, "location", res);
        if (!location) return;
        replyStation(res, req.matches[1], platform.stations.bindImageSource(req.matches[1], *kind, *location));
    });
    // 开始 / 停止持续运行：工位从自己的图像源取图、检测
    server.Post("/api/stations/" + id + "/start", [this, replyStation](const Request& req, Response& res) {
        const std::string stationId = req.matches[1];
        auto view = platform.stations.getStation(stationId);
        if (!view) return replyError(res, view.error());
        if (view.value().imageSourceKind.empty()) {
            return reply(res, 422, {{"code", "station.no_image_source"}, {"message", "工位还没有绑定图像源"}});
        }
        replyStation(res, stationId, runner.start(stationId));
    });
    server.Post("/api/stations/" + id + "/stop", [this, replyStation](const Request& req, Response& res) {
        replyStation(res, req.matches[1], runner.stop(req.matches[1]));
    });
    // 最近检测的那一帧图像
    server.Get("/api/stations/" + id + "/frame", [this](const Request& req, Response& res) {
        const Image frame = runner.latestFrame(req.matches[1]);
        if (frame.empty()) return reply(res, 404, {{"code", "api.no_frame"}, {"message", "这个工位还没有检测过图像"}});
        auto png = imaging::encodePng(frame);
        if (!png) return replyError(res, png.error());
        res.set_header("Cache-Control", "no-store");
        res.set_content(reinterpret_cast<const char*>(png.value().data()), png.value().size(), "image/png");
    });

    // ---- 检测记录（质量分析）：保存在磁盘上的，程序重启之后还在 ----
    // ?verdict=NG 只要这种判定；?before=<记录编号> 翻页，只要比它更早的；?limit=N（默认 50，最多 500）。从新到旧
    server.Get("/api/stations/" + id + "/records", [this](const Request& req, Response& res) {
        const std::string stationId = req.matches[1];
        if (auto view = platform.stations.getStation(stationId); !view) return replyError(res, view.error());
        quality::RecordQuery query;
        query.verdict = req.get_param_value("verdict");
        query.beforeId = req.get_param_value("before");
        if (!query.verdict.empty() && query.verdict != "OK" && query.verdict != "NG" && query.verdict != "ERROR") {
            return replyBadRequest(res, "verdict 只能是 OK、NG 或 ERROR");
        }
        try {
            if (req.has_param("limit")) query.limit = std::stoi(req.get_param_value("limit"));
        } catch (const std::exception&) {
            return replyBadRequest(res, "limit 必须是一个数字");
        }
        json list = json::array();
        for (const auto& r : platform.quality.query(stationId, query)) {
            json reasons = json::array();
            for (const auto& reason : r.reasons) reasons.push_back({{"nodeId", reason.nodeId}, {"pass", reason.pass}, {"text", reason.text}});
            // imageId：留下了图像的话，用 GET /api/images/{imageId} 取；没有留就是空的
            list.push_back({{"id", r.id}, {"time", r.time}, {"timeMs", r.timeMs}, {"sequence", r.sequence}, {"frameName", r.frameName},
                            {"verdict", r.verdict}, {"releaseId", r.releaseId}, {"recipeVersionId", r.recipeVersionId},
                            {"imageId", r.imageSha256}, {"swapped", r.swapped}, {"totalMs", r.totalMs},
                            {"reasons", std::move(reasons)}, {"result", resultJson(r.result)}});
        }
        reply(res, 200, {{"records", std::move(list)}});
    });
    // 这个工位有记录以来的汇总：检测了多少、良率、每种缺陷出现了多少个
    server.Get("/api/stations/" + id + "/summary", [this](const Request& req, Response& res) {
        const std::string stationId = req.matches[1];
        if (auto view = platform.stations.getStation(stationId); !view) return replyError(res, view.error());
        const auto s = platform.quality.summary(stationId);
        json j = {{"total", s.total}, {"ok", s.ok}, {"ng", s.ng}, {"error", s.error}, {"defectsByCategory", s.defectsByCategory}};
        j["yield"] = s.yield ? json(*s.yield) : json(nullptr);
        reply(res, 200, j);
    });

    // ---- 模型仓库（只读）：界面里“模型”这种参数要一个可以选的列表 ----
    server.Get("/api/models", [this](const Request&, Response& res) {
        json list = json::array();
        for (const auto& modelId : platform.models.listModelVersions()) {
            auto v = platform.models.getModelVersion(modelId);
            if (!v) continue;
            list.push_back({{"id", v.value().id}, {"status", v.value().status}, {"kind", v.value().contract.kind},
                            {"inputWidth", v.value().contract.inputWidth}, {"inputHeight", v.value().contract.inputHeight},
                            {"numbers", v.value().contract.numbers}});
        }
        reply(res, 200, {{"models", std::move(list)}});
    });

    // ---- 兜底 ----
    server.set_payload_max_length(kMaxUploadBytes);
    server.set_error_handler([](const Request& req, Response& res) {
        // 路由自己已经写了回复内容的（上面的各种出错回复），不要盖掉
        if (!res.body.empty()) return;
        if (res.status == 413) {
            // HTTP 库对“表单”类型的请求体另有一个很小的上限（8 KB）。curl -d 默认就是表单类型，上传图像时最容易撞上
            return reply(res, 413, {{"code", "api.too_large"},
                                    {"message", "请求体太大（上限 64 MB）。如果内容并不大：请求头 Content-Type 不要用表单类型，"
                                                "上传图像写 image/png 之类，JSON 写 application/json"}});
        }
        if (req.path.rfind("/api/", 0) != 0) {
            res.set_content("没有这个页面。界面的文件是不是还没有编译？见 web/README.md", "text/plain; charset=utf-8");
            return;
        }
        reply(res, res.status == 200 ? 404 : res.status, {{"code", "api.no_such_endpoint"}, {"message", "没有这个接口"}});
    });
    // 处理请求时抛出了异常：服务不能因此停掉。记下日志，回复 500
    server.set_exception_handler([](const Request& req, Response& res, std::exception_ptr ep) {
        std::string what = "未知的异常";
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        log::error("http", "处理请求时出错", {{"path", req.path}, {"what", what}});
        reply(res, 500, {{"code", "api.internal"}, {"message", "服务内部出错，详情见日志"}});
    });
}

ApiServer::ApiServer(platform::Platform& platform) : impl_(std::make_unique<Impl>(platform)) { impl_->routes(); }
ApiServer::~ApiServer() { stop(); }

int ApiServer::bind(const std::string& host, int port) {
    if (port == 0) return impl_->server.bind_to_any_port(host);
    return impl_->server.bind_to_port(host, port) ? port : -1;
}

void ApiServer::serve() { impl_->server.listen_after_bind(); }

bool ApiServer::serveWebFiles(const std::string& directory) {
    // 只把这一个目录交出去；HTTP 库会拒绝带 .. 的路径
    return impl_->server.set_mount_point("/", directory);
}
void ApiServer::stop() { impl_->server.stop(); }

}  // namespace vision::runtime
