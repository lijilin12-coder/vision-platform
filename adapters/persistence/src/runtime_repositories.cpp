#include "vision/persistence/runtime_repositories.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "json_files.h"
#include "vision/persistence/json_repositories.h"

namespace vision::persistence {

using nlohmann::json;
namespace fs = std::filesystem;
using detail::readJson;
using detail::safeFileName;
using detail::writeFileAtomically;

namespace {

std::vector<std::string> listJsonStems(const fs::path& dir) {
    std::vector<std::string> ids;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return ids;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") ids.push_back(pathToUtf8(entry.path().stem()));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

const char* stateName(station::StationState s) { return station::toString(s); }

station::StationState stateFrom(const std::string& s) {
    if (s == "running") return station::StationState::Running;
    if (s == "paused") return station::StationState::Paused;
    if (s == "faulted") return station::StationState::Faulted;
    return station::StationState::Stopped;
}

}  // namespace

// ---------- JsonReleaseRepository ----------

JsonReleaseRepository::JsonReleaseRepository(fs::path root) : dir_(std::move(root) / "releases") {}

Status JsonReleaseRepository::save(const release::Release& r) {
    if (!safeFileName(r.id())) return Status::failure("persistence.bad_id", "上线包 ID 不能用作文件名：" + r.id());
    json j = {{"schema", "vision.release/0.1"},
              {"id", r.id()},
              {"recipeVersion", r.recipeVersionId()},
              {"lockedModels", r.lockedModels()},
              {"status", r.status() == release::ReleaseStatus::Withdrawn ? "withdrawn" : "active"}};
    if (const auto& a = r.acceptance()) {
        json wrong = json::array();
        for (const auto& w : a->wrongCases) {
            wrong.push_back({{"case", w.caseId}, {"image", w.imageRef}, {"truth", w.truthNg ? "NG" : "OK"}, {"observed", w.observed}});
        }
        j["acceptance"] = {{"goldenSet", a->goldenSetId}, {"goldenSetName", a->goldenSetName}, {"checkedAtMs", a->checkedAtMs},
                           {"passed", a->passed},
                           {"criteria", {{"maxMisses", a->maxMisses}, {"maxFalseAlarmRate", a->maxFalseAlarmRate}, {"maxErrors", a->maxErrors}}},
                           {"total", a->total}, {"truthOk", a->truthOk}, {"truthNg", a->truthNg}, {"misses", a->misses},
                           {"falseAlarms", a->falseAlarms}, {"errors", a->errors}, {"meanMs", a->meanMs}, {"maxMs", a->maxMs},
                           {"wrongCases", std::move(wrong)}};
    }
    return writeFileAtomically(dir_ / (r.id() + ".json"), j.dump(2, ' ', false, json::error_handler_t::replace));
}

std::optional<release::Release> JsonReleaseRepository::find(const release::ReleaseId& id) try {
    if (!safeFileName(id)) return std::nullopt;
    auto j = readJson(dir_ / (id + ".json"));
    if (!j) return std::nullopt;
    // 验收记录：以前存下的上线包文件里没有这一项——那就是“还没做过”
    std::optional<release::AcceptanceRecord> acceptance;
    if (j->contains("acceptance")) {
        const auto& a = j->at("acceptance");
        release::AcceptanceRecord r;
        r.goldenSetId = a.at("goldenSet").get<std::string>();
        r.goldenSetName = a.at("goldenSetName").get<std::string>();
        r.checkedAtMs = a.at("checkedAtMs").get<std::int64_t>();
        r.passed = a.at("passed").get<bool>();
        r.maxMisses = a.at("criteria").at("maxMisses").get<int>();
        r.maxFalseAlarmRate = a.at("criteria").at("maxFalseAlarmRate").get<double>();
        r.maxErrors = a.at("criteria").at("maxErrors").get<int>();
        r.total = a.at("total").get<int>();
        r.truthOk = a.at("truthOk").get<int>();
        r.truthNg = a.at("truthNg").get<int>();
        r.misses = a.at("misses").get<int>();
        r.falseAlarms = a.at("falseAlarms").get<int>();
        r.errors = a.at("errors").get<int>();
        r.meanMs = a.at("meanMs").get<double>();
        r.maxMs = a.at("maxMs").get<double>();
        for (const auto& w : a.at("wrongCases")) {
            r.wrongCases.push_back({w.at("case").get<std::string>(), w.at("image").get<std::string>(),
                                    w.at("truth").get<std::string>() == "NG", w.at("observed").get<std::string>()});
        }
        acceptance = std::move(r);
    }
    return release::Release::restore(
        j->at("id").get<std::string>(), j->at("recipeVersion").get<std::string>(),
        j->at("lockedModels").get<std::map<std::string, std::string>>(),
        j->at("status").get<std::string>() == "withdrawn" ? release::ReleaseStatus::Withdrawn
                                                          : release::ReleaseStatus::Active,
        std::move(acceptance));
} catch (const std::exception&) {
    return std::nullopt;  // POC：文件损坏视为不存在
}

std::vector<release::ReleaseId> JsonReleaseRepository::listIds() { return listJsonStems(dir_); }

// ---------- JsonStationRepository ----------

JsonStationRepository::JsonStationRepository(fs::path root) : dir_(std::move(root) / "stations") {}

Status JsonStationRepository::save(const station::Station& s) {
    const station::StationSnapshot snap = s.snapshot();
    if (!safeFileName(snap.id)) return Status::failure("persistence.bad_id", "工位 ID 不能用作文件名：" + snap.id);
    json j = {{"schema", "vision.station/0.1"},
              {"id", snap.id},
              {"name", snap.name},
              {"state", stateName(snap.state)},
              {"activeRelease", snap.activeRelease ? json(*snap.activeRelease) : json(nullptr)},
              {"pendingRelease", snap.pendingRelease ? json{{"release", snap.pendingRelease->releaseId},
                                                            {"ready", snap.pendingRelease->ready}}
                                                     : json(nullptr)},
              {"releaseHistory", snap.releaseHistory},
              {"policy", {{"autoRollbackAfterErrors", snap.policy.autoRollbackAfterErrors},
                          {"historyDepth", snap.policy.historyDepth}}},
              {"consecutiveErrors", snap.consecutiveErrors},
              {"sequence", snap.sequence},
              {"imageSource", snap.imageSource ? json{{"kind", snap.imageSource->kind}, {"location", snap.imageSource->location}}
                                               : json(nullptr)}};
    return writeFileAtomically(dir_ / (snap.id + ".json"), j.dump(2));
}

std::optional<station::Station> JsonStationRepository::find(const station::StationId& id) try {
    if (!safeFileName(id)) return std::nullopt;
    auto j = readJson(dir_ / (id + ".json"));
    if (!j) return std::nullopt;
    station::StationSnapshot snap;
    snap.id = j->at("id").get<std::string>();
    snap.name = j->at("name").get<std::string>();
    snap.state = stateFrom(j->at("state").get<std::string>());
    if (!j->at("activeRelease").is_null()) snap.activeRelease = j->at("activeRelease").get<std::string>();
    if (const auto& p = j->at("pendingRelease"); !p.is_null()) {
        snap.pendingRelease = station::PendingRelease{p.at("release").get<std::string>(), p.at("ready").get<bool>()};
    }
    snap.releaseHistory = j->at("releaseHistory").get<std::vector<std::string>>();
    snap.policy.autoRollbackAfterErrors = j->at("policy").at("autoRollbackAfterErrors").get<int>();
    snap.policy.historyDepth = j->at("policy").at("historyDepth").get<int>();
    snap.consecutiveErrors = j->at("consecutiveErrors").get<int>();
    snap.sequence = j->at("sequence").get<std::uint64_t>();
    // 绑定图像源是后来加的：以前保存的工位没有这一项
    if (const auto src = j->find("imageSource"); src != j->end() && src->is_object()) {
        snap.imageSource = station::ImageSourceBinding{src->at("kind").get<std::string>(), src->at("location").get<std::string>()};
    }
    return station::Station::restore(std::move(snap));
} catch (const std::exception&) {
    return std::nullopt;  // POC：文件损坏视为不存在
}

std::vector<station::StationId> JsonStationRepository::listIds() { return listJsonStems(dir_); }

// ---------- JsonImportedRecipes ----------

JsonImportedRecipes::JsonImportedRecipes(fs::path root) : dir_(std::move(root) / "imported-recipes") {}

Status JsonImportedRecipes::save(const authoring::RecipeVersionDocument& recipe) {
    if (!safeFileName(recipe.versionId)) return Status::failure("persistence.bad_id", "方案版本 ID 不能用作文件名：" + recipe.versionId);
    return writeFileAtomically(dir_ / (recipe.versionId + ".json"), toJson(recipe, 2));
}

std::optional<authoring::RecipeVersionDocument> JsonImportedRecipes::find(const std::string& recipeVersionId) const {
    if (!safeFileName(recipeVersionId)) return std::nullopt;
    std::ifstream in(dir_ / pathFromUtf8(recipeVersionId + ".json"), std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto doc = recipeDocumentFromJson(text);
    if (!doc) return std::nullopt;  // 文件损坏视为不存在
    return std::move(doc).value();
}

std::vector<std::string> JsonImportedRecipes::listIds() const { return listJsonStems(dir_); }

// ---------- JsonlInspectionRecordRepository ----------

namespace {

json resultToJson(const ResultItems& r) {
    json defects = json::array(), measurements = json::array(), poses = json::array();
    for (const auto& d : r.defects) {
        defects.push_back({{"node", d.nodeId}, {"category", d.category}, {"cx", d.centerX}, {"cy", d.centerY}, {"left", d.left},
                           {"top", d.top}, {"width", d.width}, {"height", d.height}, {"area", d.area}});
    }
    for (const auto& m : r.measurements) {
        json j = {{"node", m.nodeId}, {"name", m.name}, {"value", m.value}, {"unit", m.unit}, {"pass", m.pass}};
        if (m.lower) j["lower"] = *m.lower;
        if (m.upper) j["upper"] = *m.upper;
        measurements.push_back(std::move(j));
    }
    for (const auto& p : r.poses) {
        poses.push_back({{"node", p.nodeId}, {"found", p.found}, {"score", p.score}, {"x", p.pose.x}, {"y", p.pose.y},
                         {"angle", p.pose.angle}, {"rx", p.pose.referenceX}, {"ry", p.pose.referenceY}});
    }
    return {{"defects", std::move(defects)}, {"measurements", std::move(measurements)}, {"poses", std::move(poses)}};
}

ResultItems resultFromJson(const json& j) {
    ResultItems r;
    for (const auto& d : j.at("defects")) {
        Defect x;
        x.nodeId = d.at("node").get<std::string>();
        x.category = d.at("category").get<std::string>();
        x.centerX = d.at("cx").get<double>();
        x.centerY = d.at("cy").get<double>();
        x.left = d.at("left").get<int>();
        x.top = d.at("top").get<int>();
        x.width = d.at("width").get<int>();
        x.height = d.at("height").get<int>();
        x.area = d.at("area").get<double>();
        r.defects.push_back(std::move(x));
    }
    for (const auto& m : j.at("measurements")) {
        Measurement x;
        x.nodeId = m.at("node").get<std::string>();
        x.name = m.at("name").get<std::string>();
        x.value = m.at("value").get<double>();
        x.unit = m.at("unit").get<std::string>();
        x.pass = m.at("pass").get<bool>();
        if (m.contains("lower")) x.lower = m.at("lower").get<double>();
        if (m.contains("upper")) x.upper = m.at("upper").get<double>();
        r.measurements.push_back(std::move(x));
    }
    for (const auto& p : j.at("poses")) {
        PoseResult x;
        x.nodeId = p.at("node").get<std::string>();
        x.found = p.at("found").get<bool>();
        x.score = p.at("score").get<double>();
        x.pose = Pose{p.at("x").get<double>(), p.at("y").get<double>(), p.at("angle").get<double>(), p.at("rx").get<double>(),
                      p.at("ry").get<double>()};
        r.poses.push_back(std::move(x));
    }
    return r;
}

// 一行文字 → 一条记录。这一行坏了（写到一半断电）就返回空，别的行不受影响
std::optional<quality::InspectionRecord> recordFromLine(const std::string& line) {
    try {
        const json j = json::parse(line);
        quality::InspectionRecord::Fields f;
        f.id = j.at("id").get<std::string>();
        f.stationId = j.at("station").get<std::string>();
        f.timeMs = j.at("timeMs").get<std::int64_t>();
        f.sequence = j.at("sequence").get<std::uint64_t>();
        f.releaseId = j.at("release").get<std::string>();
        f.recipeVersionId = j.at("recipeVersion").get<std::string>();
        f.frameName = j.at("frame").get<std::string>();
        f.imageSha256 = j.at("image").get<std::string>();
        f.verdict = j.at("verdict").get<std::string>();
        for (const auto& r : j.at("reasons")) {
            f.reasons.push_back({r.at("node").get<std::string>(), r.at("pass").get<bool>(), r.at("text").get<std::string>()});
        }
        f.result = resultFromJson(j.at("result"));
        f.totalMs = j.at("totalMs").get<double>();
        f.swapped = j.at("swapped").get<bool>();
        auto record = quality::InspectionRecord::create(std::move(f));
        if (!record) return std::nullopt;
        return std::move(record).value();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// 文件里的全部记录，按写入的顺序（从旧到新）
std::vector<quality::InspectionRecord> readRecords(const fs::path& file) {
    std::vector<quality::InspectionRecord> records;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (auto r = recordFromLine(line)) records.push_back(std::move(*r));
    }
    return records;
}

// 毫秒时间 → "2026-11-24"（UTC）
std::string dayOf(std::int64_t timeMs) {
    const std::int64_t days = timeMs / 86400000;
    // 公历日期的换算（Howard Hinnant 的算法）：不依赖系统的时区设置
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d", year, month, day);
    return buffer;
}

}  // namespace

JsonlInspectionRecordRepository::JsonlInspectionRecordRepository(fs::path root) : dir_(std::move(root) / "inspection-records") {}

Status JsonlInspectionRecordRepository::append(const quality::InspectionRecord& record) {
    const auto& f = record.fields();
    if (!safeFileName(f.stationId)) return Status::failure("persistence.bad_id", "工位 ID 不能用作目录名：" + f.stationId);
    json reasons = json::array();
    for (const auto& r : f.reasons) reasons.push_back({{"node", r.nodeId}, {"pass", r.pass}, {"text", r.text}});
    const json j = {{"id", f.id}, {"station", f.stationId}, {"timeMs", f.timeMs}, {"sequence", f.sequence}, {"release", f.releaseId},
                    {"recipeVersion", f.recipeVersionId}, {"frame", f.frameName}, {"image", f.imageSha256}, {"verdict", f.verdict},
                    {"reasons", std::move(reasons)}, {"result", resultToJson(f.result)}, {"totalMs", f.totalMs}, {"swapped", f.swapped}};
    const fs::path dir = dir_ / pathFromUtf8(f.stationId);
    std::error_code ec;
    fs::create_directories(dir, ec);
    // 追加一行。dump 不会产生换行，所以一行就是一条记录；文字里不是合法 UTF-8 的地方换成替代符号，而不是抛异常
    const fs::path file = dir / (dayOf(f.timeMs) + ".jsonl");
    // 上一次写到一半就断电了的话，文件的最后一行是残缺的、没有换行符。
    // 先补一个换行，不然这一条会接在那半行后面，两条一起作废。
    bool torn = false;
    {
        std::ifstream last(file, std::ios::binary | std::ios::ate);
        if (last && last.tellg() > 0) {
            last.seekg(-1, std::ios::end);
            torn = last.get() != '\n';
        }
    }
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (torn) out << '\n';
    out << j.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
    out.flush();
    if (!out) return Status::failure("persistence.write", "检测记录写不进去（磁盘满了？）");
    return Status::success();
}

std::vector<fs::path> JsonlInspectionRecordRepository::files(const std::string& stationId) const {
    std::vector<fs::path> found;
    if (!safeFileName(stationId)) return found;
    std::error_code ec;
    for (fs::directory_iterator it(dir_ / pathFromUtf8(stationId), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".jsonl") found.push_back(it->path());
    }
    std::sort(found.begin(), found.end(), std::greater<>());  // 文件名是日期：倒序就是从新到旧
    return found;
}

std::vector<quality::InspectionRecord> JsonlInspectionRecordRepository::query(const std::string& stationId,
                                                                               const quality::RecordFilter& filter) {
    std::vector<quality::InspectionRecord> result;
    for (const auto& file : files(stationId)) {
        auto records = readRecords(file);
        // 同一个文件里按编号从新到旧（时钟往回走过的话，写入的顺序不一定就是编号的顺序）
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a->id > b->id; });
        for (auto& r : records) {
            if (!filter.beforeId.empty() && r->id >= filter.beforeId) continue;
            if (!filter.verdict.empty() && r->verdict != filter.verdict) continue;
            result.push_back(std::move(r));
            if (static_cast<int>(result.size()) >= filter.limit) return result;
        }
    }
    return result;
}

std::optional<quality::InspectionRecord> JsonlInspectionRecordRepository::find(const std::string& stationId, const std::string& recordId) {
    // 编号的前 13 位是时间：直接知道它在哪一天的文件里
    std::int64_t timeMs = 0;
    try {
        timeMs = std::stoll(recordId.substr(0, 13));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!safeFileName(stationId)) return std::nullopt;
    for (auto& r : readRecords(dir_ / pathFromUtf8(stationId) / (dayOf(timeMs) + ".jsonl"))) {
        if (r->id == recordId) return std::move(r);
    }
    return std::nullopt;
}

std::vector<quality::InspectionRecord> JsonlInspectionRecordRepository::all(const std::string& stationId) {
    std::vector<quality::InspectionRecord> result;
    for (const auto& file : files(stationId)) {
        auto records = readRecords(file);
        result.insert(result.end(), std::make_move_iterator(records.begin()), std::make_move_iterator(records.end()));
    }
    return result;
}

// ---------- JsonGoldenSetRepository ----------

JsonGoldenSetRepository::JsonGoldenSetRepository(fs::path root) : dir_(std::move(root) / "golden-sets") {}

Status JsonGoldenSetRepository::save(const release::GoldenSet& s) {
    if (!safeFileName(s.id())) return Status::failure("persistence.bad_id", "基准图集 ID 不能用作文件名：" + s.id());
    json cases = json::array();
    for (const auto& c : s.cases()) {
        cases.push_back({{"id", c.id}, {"image", c.imageRef}, {"truth", c.truth == release::Truth::Ng ? "NG" : "OK"},
                         {"sample", c.sampleId}});
    }
    json j = {{"schema", "vision.golden-set/0.1"},
              {"id", s.id()},
              {"name", s.name()},
              {"frozen", s.frozen()},
              {"criteria", {{"maxMisses", s.criteria().maxMisses},
                            {"maxFalseAlarmRate", s.criteria().maxFalseAlarmRate},
                            {"maxErrors", s.criteria().maxErrors}}},
              {"cases", cases}};
    return writeFileAtomically(dir_ / (s.id() + ".json"), j.dump(2));
}

std::optional<release::GoldenSet> JsonGoldenSetRepository::find(const release::GoldenSetId& id) try {
    if (!safeFileName(id)) return std::nullopt;
    auto j = readJson(dir_ / (id + ".json"));
    if (!j) return std::nullopt;
    std::vector<release::GoldenCase> cases;
    for (const auto& c : j->at("cases")) {
        cases.push_back({c.at("id").get<std::string>(), c.at("image").get<std::string>(),
                         c.at("truth").get<std::string>() == "NG" ? release::Truth::Ng : release::Truth::Ok,
                         c.value("sample", std::string())});  // 旧文件里没有 sample
    }
    const json& k = j->at("criteria");
    return release::GoldenSet::restore(j->at("id").get<std::string>(), j->at("name").get<std::string>(), std::move(cases),
                                       {k.at("maxMisses").get<int>(), k.at("maxFalseAlarmRate").get<double>(),
                                        k.at("maxErrors").get<int>()},
                                       j->at("frozen").get<bool>());
} catch (const std::exception&) {
    return std::nullopt;  // POC：文件损坏视为不存在
}

std::vector<release::GoldenSetId> JsonGoldenSetRepository::listIds() { return listJsonStems(dir_); }

// ---------- JsonSampleRepository ----------

JsonSampleRepository::JsonSampleRepository(fs::path root) : dir_(std::move(root) / "samples") {}

Status JsonSampleRepository::save(const data::Sample& s) {
    if (!safeFileName(s.id())) return Status::failure("persistence.bad_id", "样本 ID 不能用作文件名");
    const json j = {{"schema", "vision.sample/0.1"}, {"id", s.id()}, {"bytes", s.bytes()}, {"names", s.names()},
                    {"label", data::toString(s.label())}, {"use", data::toString(s.use())}};
    return writeFileAtomically(dir_ / (s.id() + ".json"), j.dump(2));
}

namespace {
data::Label labelFrom(const std::string& s) { return s == "ok" ? data::Label::Ok : s == "ng" ? data::Label::Ng : data::Label::Unlabeled; }
data::SampleUse useFrom(const std::string& s) {
    return s == "benchmark" ? data::SampleUse::Benchmark : s == "training" ? data::SampleUse::Training : data::SampleUse::Unassigned;
}
}  // namespace

std::optional<data::Sample> JsonSampleRepository::find(const data::SampleId& id) try {
    if (!safeFileName(id)) return std::nullopt;
    auto j = readJson(dir_ / (id + ".json"));
    if (!j) return std::nullopt;
    return data::Sample::restore(j->at("id").get<std::string>(), j->at("bytes").get<long long>(),
                                 j->at("names").get<std::vector<std::string>>(), labelFrom(j->at("label").get<std::string>()),
                                 useFrom(j->at("use").get<std::string>()));
} catch (const std::exception&) {
    return std::nullopt;
}

std::vector<data::SampleId> JsonSampleRepository::listIds() { return listJsonStems(dir_); }

// ---------- JsonDatasetRepository ----------

JsonDatasetRepository::JsonDatasetRepository(fs::path root) : dir_(root / "datasets"), versionsDir_(root / "dataset-versions") {}

Status JsonDatasetRepository::save(const data::Dataset& d) {
    if (!safeFileName(d.id())) return Status::failure("persistence.bad_id", "数据集 ID 不能用作文件名");
    const json j = {{"schema", "vision.dataset/0.1"}, {"id", d.id()}, {"name", d.name()}, {"samples", d.samples()},
                    {"nextVersion", d.nextVersion()}};
    return writeFileAtomically(dir_ / (d.id() + ".json"), j.dump(2));
}

std::optional<data::Dataset> JsonDatasetRepository::find(const data::DatasetId& id) try {
    if (!safeFileName(id)) return std::nullopt;
    auto j = readJson(dir_ / (id + ".json"));
    if (!j) return std::nullopt;
    return data::Dataset::restore(j->at("id").get<std::string>(), j->at("name").get<std::string>(),
                                  j->at("samples").get<std::vector<std::string>>(), j->at("nextVersion").get<int>());
} catch (const std::exception&) {
    return std::nullopt;
}

std::vector<data::DatasetId> JsonDatasetRepository::listIds() { return listJsonStems(dir_); }

// 数据集版本的 ID 形如 "<数据集ID>@v1"，存成 <数据集ID>/v1.json
Status JsonDatasetRepository::saveVersion(const data::DatasetVersion& v) {
    if (!safeFileName(v.dataset())) return Status::failure("persistence.bad_id", "数据集 ID 不能用作文件名");
    json entries = json::array();
    for (const auto& e : v.entries()) entries.push_back({{"sample", e.sampleId}, {"label", data::toString(e.label)}});
    const json j = {{"schema", "vision.dataset-version/0.1"}, {"id", v.id()}, {"dataset", v.dataset()}, {"number", v.number()},
                    {"name", v.name()}, {"entries", entries}};
    return writeFileAtomically(versionsDir_ / v.dataset() / ("v" + std::to_string(v.number()) + ".json"), j.dump(2));
}

std::optional<data::DatasetVersion> JsonDatasetRepository::findVersion(const std::string& versionId) try {
    const auto at = versionId.rfind("@v");
    if (at == std::string::npos) return std::nullopt;
    const std::string dataset = versionId.substr(0, at), number = versionId.substr(at + 2);
    if (!safeFileName(dataset) || !safeFileName(number)) return std::nullopt;
    auto j = readJson(versionsDir_ / dataset / ("v" + number + ".json"));
    if (!j) return std::nullopt;
    std::vector<data::DatasetEntry> entries;
    for (const auto& e : j->at("entries")) entries.push_back({e.at("sample").get<std::string>(), labelFrom(e.at("label").get<std::string>())});
    return data::DatasetVersion(j->at("id").get<std::string>(), j->at("dataset").get<std::string>(), j->at("number").get<int>(),
                                j->at("name").get<std::string>(), std::move(entries));
} catch (const std::exception&) {
    return std::nullopt;
}

// ---------- JsonModelVersionRepository ----------

JsonModelVersionRepository::JsonModelVersionRepository(fs::path root) : dir_(std::move(root) / "model-versions") {}

Status JsonModelVersionRepository::save(const models::ModelVersion& v) {
    if (!safeFileName(v.id())) return Status::failure("persistence.bad_id", "模型版本 ID 不能用作文件名：" + v.id());
    const auto& c = v.contract();
    const auto& p = v.provenance();
    const json j = {{"schema", "vision.model-version/0.1"},
                    {"model", v.modelName()},
                    {"number", v.number()},
                    {"sha256", v.contentHash()},
                    {"bytes", v.bytes()},
                    {"status", v.status() == models::ModelStatus::Published ? "published" : "unpublished"},
                    {"contract", {{"kind", c.kind}, {"inputWidth", c.inputWidth}, {"inputHeight", c.inputHeight},
                                  {"inputChannels", c.inputChannels}, {"output", c.output}, {"numbers", c.numbers}}},
                    {"provenance", {{"kind", p.kind}, {"trainingJob", p.trainingJobId}, {"datasetVersion", p.datasetVersionId},
                                    {"split", p.split}, {"backend", p.backend}}}};
    return writeFileAtomically(dir_ / (v.id() + ".json"), j.dump(2));
}

std::optional<models::ModelVersion> JsonModelVersionRepository::find(const std::string& id) try {
    if (!safeFileName(id)) return std::nullopt;
    auto j = readJson(dir_ / (id + ".json"));
    if (!j) return std::nullopt;
    const json& c = j->at("contract");
    const json& p = j->at("provenance");
    return models::ModelVersion::restore(
        j->at("model").get<std::string>(), j->at("number").get<int>(), j->at("sha256").get<std::string>(), j->at("bytes").get<long long>(),
        {c.at("kind").get<std::string>(), c.at("inputWidth").get<int>(), c.at("inputHeight").get<int>(), c.at("inputChannels").get<int>(),
         c.at("output").get<std::string>(), c.at("numbers").get<std::map<std::string, double>>()},
        {p.at("kind").get<std::string>(), p.at("trainingJob").get<std::string>(), p.at("datasetVersion").get<std::string>(),
         p.at("split").get<std::string>(), p.at("backend").get<std::string>()},
        j->at("status").get<std::string>() == "published" ? models::ModelStatus::Published : models::ModelStatus::Unpublished);
} catch (const std::exception&) {
    return std::nullopt;
}

std::vector<std::string> JsonModelVersionRepository::listIds() { return listJsonStems(dir_); }

// ---------- JsonTrainingJobRepository ----------

JsonTrainingJobRepository::JsonTrainingJobRepository(fs::path root) : dir_(std::move(root) / "training-jobs") {}

Status JsonTrainingJobRepository::save(const training::TrainingJob& job) {
    const training::JobState& s = job.state();
    if (!safeFileName(s.id)) return Status::failure("persistence.bad_id", "训练任务 ID 不能用作文件名：" + s.id);
    json j = {{"schema", "vision.training-job/0.1"},
              {"id", s.id},
              {"datasetVersion", s.datasetVersionId},
              {"config", {{"modelKind", s.config.modelKind}, {"modelName", s.config.modelName}, {"settings", s.config.settings}}},
              {"split", {{"train", s.split.train}, {"validation", s.split.validation}, {"test", s.split.test}, {"seed", s.split.seed}}},
              {"backend", s.backend},
              {"backendTicket", s.backendTicket},
              {"status", training::toString(s.status)},
              {"failure", s.failure},
              {"modelVersion", s.modelVersionId},
              {"report", nullptr},
              {"output", nullptr}};
    if (s.report) j["report"] = {{"metrics", s.report->metrics}, {"seconds", s.report->seconds}, {"notes", s.report->notes}};
    if (s.output) {
        j["output"] = {{"modelFile", s.output->modelFile}, {"contractKind", s.output->contractKind}, {"inputWidth", s.output->inputWidth},
                       {"inputHeight", s.output->inputHeight}, {"inputChannels", s.output->inputChannels},
                       {"outputMeaning", s.output->outputMeaning}, {"numbers", s.output->numbers}};
    }
    return writeFileAtomically(dir_ / (s.id + ".json"), j.dump(2));
}

std::optional<training::TrainingJob> JsonTrainingJobRepository::find(const training::TrainingJobId& id) try {
    if (!safeFileName(id)) return std::nullopt;
    auto read = readJson(dir_ / (id + ".json"));
    if (!read) return std::nullopt;
    const json& j = *read;
    training::JobState s;
    s.id = j.at("id").get<std::string>();
    s.datasetVersionId = j.at("datasetVersion").get<std::string>();
    s.config = {j.at("config").at("modelKind").get<std::string>(), j.at("config").at("modelName").get<std::string>(),
                j.at("config").at("settings").get<std::map<std::string, double>>()};
    s.split = {j.at("split").at("train").get<double>(), j.at("split").at("validation").get<double>(), j.at("split").at("test").get<double>(),
               j.at("split").at("seed").get<unsigned>()};
    s.backend = j.at("backend").get<std::string>();
    s.backendTicket = j.at("backendTicket").get<std::string>();
    const std::string status = j.at("status").get<std::string>();
    for (const auto candidate : {training::JobStatus::Submitted, training::JobStatus::Running, training::JobStatus::Completed,
                                 training::JobStatus::Failed, training::JobStatus::Cancelled}) {
        if (status == training::toString(candidate)) s.status = candidate;
    }
    s.failure = j.at("failure").get<std::string>();
    s.modelVersionId = j.at("modelVersion").get<std::string>();
    if (!j.at("report").is_null()) {
        const json& r = j.at("report");
        s.report = training::TrainingReport{r.at("metrics").get<std::map<std::string, double>>(), r.at("seconds").get<double>(),
                                            r.at("notes").get<std::string>()};
    }
    if (!j.at("output").is_null()) {
        const json& o = j.at("output");
        s.output = training::TrainingOutput{o.at("modelFile").get<std::string>(), o.at("contractKind").get<std::string>(),
                                            o.at("inputWidth").get<int>(), o.at("inputHeight").get<int>(), o.at("inputChannels").get<int>(),
                                            o.at("outputMeaning").get<std::string>(), o.at("numbers").get<std::map<std::string, double>>()};
    }
    return training::TrainingJob::restore(std::move(s));
} catch (const std::exception&) {
    return std::nullopt;
}

std::vector<training::TrainingJobId> JsonTrainingJobRepository::listIds() { return listJsonStems(dir_); }

}  // namespace vision::persistence
