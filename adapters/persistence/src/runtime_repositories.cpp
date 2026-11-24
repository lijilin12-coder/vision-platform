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
