#include "vision/persistence/runtime_repositories.h"

#include <algorithm>

#include "json_files.h"

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
    return writeFileAtomically(dir_ / (r.id() + ".json"), j.dump(2));
}

std::optional<release::Release> JsonReleaseRepository::find(const release::ReleaseId& id) try {
    if (!safeFileName(id)) return std::nullopt;
    auto j = readJson(dir_ / (id + ".json"));
    if (!j) return std::nullopt;
    return release::Release::restore(
        j->at("id").get<std::string>(), j->at("recipeVersion").get<std::string>(),
        j->at("lockedModels").get<std::map<std::string, std::string>>(),
        j->at("status").get<std::string>() == "withdrawn" ? release::ReleaseStatus::Withdrawn
                                                          : release::ReleaseStatus::Active);
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
              {"sequence", snap.sequence}};
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
    return station::Station::restore(std::move(snap));
} catch (const std::exception&) {
    return std::nullopt;  // POC：文件损坏视为不存在
}

std::vector<station::StationId> JsonStationRepository::listIds() { return listJsonStems(dir_); }

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

}  // namespace vision::persistence
