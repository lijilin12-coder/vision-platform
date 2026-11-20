#include "vision/persistence/json_repositories.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "json_files.h"
#include "vision/common/path.h"

namespace vision::persistence {

using nlohmann::json;
namespace fs = std::filesystem;
using namespace vision::authoring;
using detail::readJson;
using detail::writeFileAtomically;

namespace {

// ---------- 基础类型 ----------

json paramToJson(const ParamValue& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
    return std::get<std::string>(v);
}

ParamValue paramFromJson(const json& j) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<double>();
    return j.get<std::string>();
}

json paramsToJson(const ParamMap& m) {
    json j = json::object();
    for (const auto& [k, v] : m) j[k] = paramToJson(v);
    return j;
}

ParamMap paramsFromJson(const json& j) {
    ParamMap m;
    for (auto it = j.begin(); it != j.end(); ++it) m[it.key()] = paramFromJson(it.value());
    return m;
}

json roiToJson(const Roi& r) {
    return {{"type", "rect"}, {"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}};
}

Roi roiFromJson(const json& j) {
    auto r = Roi::rect(j.at("x").get<double>(), j.at("y").get<double>(), j.at("width").get<double>(),
                       j.at("height").get<double>());
    if (!r) throw std::runtime_error(r.error().message);
    return r.value();
}

const char* paramTypeName(ParamType t) {
    switch (t) {
        case ParamType::Number: return "number";
        case ParamType::Bool: return "bool";
        case ParamType::Text: return "text";
        case ParamType::Choice: return "choice";
    }
    return "number";
}

ParamType paramTypeFrom(const std::string& s) {
    if (s == "bool") return ParamType::Bool;
    if (s == "text") return ParamType::Text;
    if (s == "choice") return ParamType::Choice;
    return ParamType::Number;
}

json portsToJson(const std::vector<PortDef>& ports) {
    json a = json::array();
    for (const auto& p : ports) a.push_back({{"name", p.name}, {"type", toString(p.type)}, {"optional", p.optional}});
    return a;
}

std::vector<PortDef> portsFromJson(const json& a) {
    std::vector<PortDef> out;
    for (const auto& p : a) {
        auto t = parsePortType(p.at("type").get<std::string>());
        if (!t) throw std::runtime_error("未知端口类型");
        out.push_back(PortDef{p.at("name").get<std::string>(), *t, p.value("optional", false)});
    }
    return out;
}

json signatureToJson(const OperatorSignature& s) {
    json params = json::array();
    for (const auto& p : s.params) {
        json jp = {{"name", p.name}, {"type", paramTypeName(p.type)}, {"default", paramToJson(p.defaultValue)},
                   {"label", p.label}};
        if (p.min) jp["min"] = *p.min;
        if (p.max) jp["max"] = *p.max;
        if (!p.choices.empty()) jp["choices"] = p.choices;
        params.push_back(jp);
    }
    return {{"id", s.id}, {"majorVersion", s.majorVersion}, {"inputs", portsToJson(s.inputs)},
            {"outputs", portsToJson(s.outputs)}, {"params", params}};
}

OperatorSignature signatureFromJson(const json& j) {
    OperatorSignature s;
    s.id = j.at("id").get<std::string>();
    s.majorVersion = j.at("majorVersion").get<int>();
    s.inputs = portsFromJson(j.at("inputs"));
    s.outputs = portsFromJson(j.at("outputs"));
    for (const auto& jp : j.at("params")) {
        ParamDef p;
        p.name = jp.at("name").get<std::string>();
        p.type = paramTypeFrom(jp.at("type").get<std::string>());
        p.defaultValue = paramFromJson(jp.at("default"));
        p.label = jp.value("label", "");
        if (jp.contains("min")) p.min = jp["min"].get<double>();
        if (jp.contains("max")) p.max = jp["max"].get<double>();
        if (jp.contains("choices")) p.choices = jp["choices"].get<std::vector<std::string>>();
        s.params.push_back(p);
    }
    return s;
}

// ---------- 方案编排的实体 ----------

json nodeToJson(const Node& n) {
    json j = {{"id", n.id}, {"operator", signatureToJson(n.op)}, {"params", paramsToJson(n.params)},
              {"layout", {{"x", n.layout.x}, {"y", n.layout.y}}}};
    switch (n.roi.kind()) {
        case RoiSpec::Kind::None: break;
        case RoiSpec::Kind::Absolute: j["roi"] = {{"kind", "absolute"}, {"roi", roiToJson(*n.roi.roi())}}; break;
        case RoiSpec::Kind::Named: j["roi"] = {{"kind", "named"}, {"region", n.roi.region()}}; break;
    }
    return j;
}

Node nodeFromJson(const json& j) {
    Node n;
    n.id = j.at("id").get<std::string>();
    n.op = signatureFromJson(j.at("operator"));
    n.params = paramsFromJson(j.at("params"));
    if (j.contains("layout")) n.layout = NodeLayout{j["layout"].value("x", 0.0), j["layout"].value("y", 0.0)};
    if (j.contains("roi")) {
        const auto kind = j["roi"].at("kind").get<std::string>();
        if (kind == "absolute") n.roi = RoiSpec::absolute(roiFromJson(j["roi"].at("roi")));
        if (kind == "named") n.roi = RoiSpec::named(j["roi"].at("region").get<std::string>());
    }
    return n;
}

json edgesToJson(const std::vector<Edge>& edges) {
    json a = json::array();
    for (const auto& e : edges) a.push_back({{"from", e.from.str()}, {"to", e.to.str()}});
    return a;
}

std::vector<Edge> edgesFromJson(const json& a) {
    std::vector<Edge> out;
    for (const auto& e : a) {
        auto f = PortRef::parse(e.at("from").get<std::string>());
        auto t = PortRef::parse(e.at("to").get<std::string>());
        if (!f || !t) throw std::runtime_error("连线格式错误");
        out.push_back(Edge{f.value(), t.value()});
    }
    return out;
}

json regionsToJson(const std::vector<NamedRegion>& regions) {
    json a = json::array();
    for (const auto& r : regions) a.push_back({{"id", r.id}, {"name", r.name}, {"roi", roiToJson(r.roi)}});
    return a;
}

std::vector<NamedRegion> regionsFromJson(const json& a) {
    std::vector<NamedRegion> out;
    for (const auto& r : a) {
        out.push_back(NamedRegion{r.at("id").get<std::string>(), r.at("name").get<std::string>(), roiFromJson(r.at("roi"))});
    }
    return out;
}

}  // namespace

// ---------- JsonRecipeRepository ----------

JsonRecipeRepository::JsonRecipeRepository(fs::path root) : dir_(std::move(root) / "recipes") {}

Status JsonRecipeRepository::save(const Recipe& r) {
    json nodes = json::array();
    for (const auto& n : r.nodes()) nodes.push_back(nodeToJson(n));
    json j = {{"schema", "vision.recipe/0.1"},   {"id", r.id()},
              {"name", r.name()},                {"nextNodeNumber", r.nextNodeNumber()},
              {"nextVersionNumber", r.nextVersionNumber()}, {"nodes", nodes},
              {"edges", edgesToJson(r.edges())}, {"regions", regionsToJson(r.regions())}};
    return writeFileAtomically(dir_ / (r.id() + ".json"), j.dump(2));
}

std::optional<Recipe> JsonRecipeRepository::find(const RecipeId& id) try {
    if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos) return std::nullopt;
    auto j = readJson(dir_ / (id + ".json"));
    if (!j) return std::nullopt;
    std::vector<Node> nodes;
    for (const auto& n : j->at("nodes")) nodes.push_back(nodeFromJson(n));
    return Recipe::restore(j->at("id").get<std::string>(), j->at("name").get<std::string>(),
                           j->at("nextNodeNumber").get<int>(), j->at("nextVersionNumber").get<int>(), std::move(nodes),
                           edgesFromJson(j->at("edges")), regionsFromJson(j->at("regions")));
} catch (const std::exception&) {
    return std::nullopt;  // POC：文件损坏视为不存在。正式版应返回错误并记录日志
}

std::vector<RecipeId> JsonRecipeRepository::listIds() {
    std::vector<RecipeId> ids;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return ids;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (entry.path().extension() == ".json") ids.push_back(pathToUtf8(entry.path().stem()));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// ---------- JsonRecipeVersionRepository ----------

JsonRecipeVersionRepository::JsonRecipeVersionRepository(fs::path root) : dir_(std::move(root) / "recipe-versions") {}

Status JsonRecipeVersionRepository::save(const RecipeVersion& v) {
    json nodes = json::array();
    for (const auto& n : v.nodes()) nodes.push_back(nodeToJson(n));
    json j = {{"schema", "vision.recipe-version/0.1"},
              {"id", v.id().str()},
              {"recipeName", v.recipeName()},
              {"status", v.status() == VersionStatus::Active ? "active" : "deprecated"},
              {"nodes", nodes},
              {"edges", edgesToJson(v.edges())},
              {"regions", regionsToJson(v.regions())}};
    return writeFileAtomically(dir_ / v.id().recipe / ("v" + std::to_string(v.id().number) + ".json"), j.dump(2));
}

std::optional<RecipeVersion> JsonRecipeVersionRepository::find(const RecipeVersionId& id) try {
    if (id.recipe.find('/') != std::string::npos || id.recipe.find('\\') != std::string::npos) return std::nullopt;
    auto j = readJson(dir_ / id.recipe / ("v" + std::to_string(id.number) + ".json"));
    if (!j) return std::nullopt;
    std::vector<Node> nodes;
    for (const auto& n : j->at("nodes")) nodes.push_back(nodeFromJson(n));
    const auto status = j->at("status").get<std::string>() == "deprecated" ? VersionStatus::Deprecated
                                                                            : VersionStatus::Active;
    return RecipeVersion(id, j->at("recipeName").get<std::string>(), std::move(nodes), edgesFromJson(j->at("edges")),
                         regionsFromJson(j->at("regions")), status);
} catch (const std::exception&) {
    return std::nullopt;
}

// ---------- 发布语言 ----------

std::string toJson(const RecipeVersionDocument& doc, int indent) {
    json nodes = json::array();
    for (const auto& n : doc.nodes) {
        json jn = {{"id", n.id}, {"operator", n.operatorId}, {"operatorMajor", n.operatorMajor},
                   {"params", paramsToJson(n.params)}};
        if (n.roi) jn["roi"] = roiToJson(*n.roi);
        nodes.push_back(jn);
    }
    json edges = json::array();
    for (const auto& e : doc.edges) edges.push_back({{"from", e.from}, {"to", e.to}});
    json j = {{"schemaVersion", doc.schemaVersion}, {"versionId", doc.versionId}, {"recipeName", doc.recipeName},
              {"nodes", nodes}, {"edges", edges}};
    return j.dump(indent);
}

Result<RecipeVersionDocument> recipeDocumentFromJson(const std::string& text) {
    try {
        const json j = json::parse(text);
        RecipeVersionDocument doc;
        doc.schemaVersion = j.at("schemaVersion").get<std::string>();
        doc.versionId = j.at("versionId").get<std::string>();
        doc.recipeName = j.value("recipeName", "");
        for (const auto& jn : j.at("nodes")) {
            RecipeDocumentNode n;
            n.id = jn.at("id").get<std::string>();
            n.operatorId = jn.at("operator").get<std::string>();
            n.operatorMajor = jn.at("operatorMajor").get<int>();
            n.params = paramsFromJson(jn.at("params"));
            if (jn.contains("roi")) n.roi = roiFromJson(jn["roi"]);
            doc.nodes.push_back(std::move(n));
        }
        for (const auto& je : j.at("edges")) doc.edges.push_back({je.at("from").get<std::string>(), je.at("to").get<std::string>()});
        return doc;
    } catch (const std::exception& e) {
        return Result<RecipeVersionDocument>::failure("persistence.parse", std::string("方案文档解析失败：") + e.what());
    }
}

}  // namespace vision::persistence
