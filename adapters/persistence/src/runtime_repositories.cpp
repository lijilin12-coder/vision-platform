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
        cases.push_back({{"id", c.id}, {"image", c.imageRef}, {"truth", c.truth == release::Truth::Ng ? "NG" : "OK"}});
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
                         c.at("truth").get<std::string>() == "NG" ? release::Truth::Ng : release::Truth::Ok});
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

}  // namespace vision::persistence
