#include "vision/training_http/http_training_backend.h"

#include <filesystem>

#include <nlohmann/json.hpp>

#include "vision/common/path.h"

namespace vision::training_http {

using nlohmann::json;

namespace {

// 训练服务回复里的出错信息（{"error": "..."}）；没有就给出状态码
std::string errorText(const http::HttpResponse& response) {
    const json body = json::parse(response.body, nullptr, false);
    if (body.is_object() && body.contains("error") && body["error"].is_string()) return body["error"].get<std::string>();
    return "训练服务回复了状态码 " + std::to_string(response.status);
}

std::map<std::string, double> numbers(const json& object) {
    std::map<std::string, double> out;
    if (!object.is_object()) return out;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.value().is_number()) out[it.key()] = it.value().get<double>();
    }
    return out;
}

std::string text(const json& object, const char* key) {
    return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string();
}

int integer(const json& object, const char* key, int fallback) {
    return object.is_object() && object.contains(key) && object[key].is_number_integer() ? object[key].get<int>() : fallback;
}

}  // namespace

HttpTrainingBackend::HttpTrainingBackend(http::HttpClient& http, std::string baseUrl) : http_(http), baseUrl_(std::move(baseUrl)) {
    while (!baseUrl_.empty() && baseUrl_.back() == '/') baseUrl_.pop_back();
}

Result<std::string> HttpTrainingBackend::start(const training::TrainingRequest& request) {
    using R = Result<std::string>;
    json body;
    body["jobId"] = request.jobId;
    body["config"] = {{"modelKind", request.config.modelKind}, {"modelName", request.config.modelName}, {"settings", request.config.settings}};
    body["samples"] = json::array();
    for (const auto& s : request.samples) {
        // 训练服务的当前目录和平台的不一样：交出去的必须是绝对路径
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(pathFromUtf8(s.contentPath), ec);
        body["samples"].push_back({{"sampleId", s.sampleId},
                                   {"ng", s.ng},
                                   {"split", training::toString(s.split)},
                                   {"path", ec ? s.contentPath : pathToUtf8(absolute)}});
    }
    http::HttpRequest http;
    http.url = baseUrl_ + "/jobs";
    http.headers = {"Content-Type: application/json"};
    http.body = body.dump();
    http.timeoutSeconds = 30;
    auto response = http_.send(http);
    if (!response) return R::failure("training.backend_unreachable", "连不上训练服务 " + baseUrl_ + "：" + response.error().message);
    if (response.value().status != 200) return R::failure("training.backend_rejected", errorText(response.value()));
    const json reply = json::parse(response.value().body, nullptr, false);
    const std::string ticket = text(reply, "ticket");
    if (ticket.empty()) return R::failure("training.backend_bad_reply", "训练服务的回复里没有训练编号");
    return ticket;
}

Result<training::BackendProgress> HttpTrainingBackend::poll(const std::string& ticket) {
    using R = Result<training::BackendProgress>;
    http::HttpRequest http;
    http.method = "GET";
    http.url = baseUrl_ + "/jobs/" + ticket;
    http.timeoutSeconds = 30;
    auto response = http_.send(http);
    if (!response) return R::failure("training.backend_unreachable", "连不上训练服务 " + baseUrl_ + "：" + response.error().message);
    training::BackendProgress progress;
    if (response.value().status == 404) {
        // 训练服务不认识这个编号：多半是它重启过，正在跑的训练丢了。这是确定的结局，不是“暂时查不到”
        progress.state = training::BackendProgress::State::Failed;
        progress.failure = "训练服务不认识这次训练（它可能重启过）";
        return progress;
    }
    if (response.value().status != 200) return R::failure("training.backend_bad_reply", errorText(response.value()));
    const json reply = json::parse(response.value().body, nullptr, false);
    const std::string state = text(reply, "state");
    if (reply.contains("progress") && reply["progress"].is_number()) progress.progress = reply["progress"].get<double>();
    if (state == "running") return progress;
    if (state == "failed") {
        progress.state = training::BackendProgress::State::Failed;
        progress.failure = text(reply, "failure");
        if (progress.failure.empty()) progress.failure = "训练服务没有说明原因";
        return progress;
    }
    if (state != "completed") return R::failure("training.backend_bad_reply", "看不懂训练服务的回复");

    const json report = reply.value("report", json::object()), output = reply.value("output", json::object());
    progress.state = training::BackendProgress::State::Completed;
    progress.progress = 1;
    progress.report.metrics = numbers(report.is_object() ? report.value("metrics", json::object()) : json::object());
    if (report.is_object() && report.contains("seconds") && report["seconds"].is_number()) progress.report.seconds = report["seconds"].get<double>();
    progress.report.notes = text(report, "notes");
    progress.output.modelFile = text(output, "modelFile");
    progress.output.contractKind = text(output, "contractKind");
    progress.output.inputWidth = integer(output, "inputWidth", 0);
    progress.output.inputHeight = integer(output, "inputHeight", 0);
    progress.output.inputChannels = integer(output, "inputChannels", 1);
    progress.output.outputMeaning = text(output, "outputMeaning");
    progress.output.numbers = numbers(output.is_object() ? output.value("numbers", json::object()) : json::object());
    if (progress.output.modelFile.empty()) return R::failure("training.backend_bad_reply", "训练服务说训练完成了，但没有给出模型文件");
    return progress;
}

Status HttpTrainingBackend::cancel(const std::string& ticket) {
    http::HttpRequest http;
    http.method = "DELETE";
    http.url = baseUrl_ + "/jobs/" + ticket;
    http.timeoutSeconds = 30;
    auto response = http_.send(http);
    if (!response) return Status::failure("training.backend_unreachable", "连不上训练服务 " + baseUrl_ + "：" + response.error().message);
    // 404：训练服务已经不记得这次训练了，也就没有什么可取消的
    if (response.value().status != 200 && response.value().status != 404) {
        return Status::failure("training.backend_bad_reply", errorText(response.value()));
    }
    return Status::success();
}

}  // namespace vision::training_http
