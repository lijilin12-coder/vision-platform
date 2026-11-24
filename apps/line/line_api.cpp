#include "line_api.h"

#include <fstream>
#include <iostream>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "vision/common/path.h"
#include "vision/http/http_client.h"

namespace vision::line {

using nlohmann::json;
using httplib::Request;
using httplib::Response;

namespace {

json stationJson(const station::StationView& v) {
    return {{"id", v.id}, {"name", v.name}, {"state", v.state}, {"activeRelease", v.activeRelease},
            {"pendingRelease", v.pendingRelease}, {"pendingReady", v.pendingReady}, {"releaseHistory", v.releaseHistory},
            {"consecutiveErrors", v.consecutiveErrors}, {"sequence", v.sequence},
            {"imageSource", {{"kind", v.imageSourceKind}, {"location", v.imageSourceLocation}}}};
}

void reply(Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json; charset=utf-8");
}

void replyError(Response& res, const Error& e) {
    reply(res, e.code.find("not_found") != std::string::npos ? 404 : 422, {{"code", e.code}, {"message", e.message}});
}

}  // namespace

struct ControlApi::Impl {
    explicit Impl(platform::LinePlatform& p) : platform(p) {}
    platform::LinePlatform& platform;
    httplib::Server server;

    void replyStation(Response& res, const std::string& id, const Status& st = Status::success()) {
        if (!st) return replyError(res, st.error());
        auto view = platform.stations.getStation(id);
        if (!view) return replyError(res, view.error());
        reply(res, 200, stationJson(view.value()));
    }
};

ControlApi::ControlApi(platform::LinePlatform& platform) : impl_(std::make_unique<Impl>(platform)) {
    auto* impl = impl_.get();
    auto& stations = platform.stations;
    auto& server = impl->server;
    const std::string id = "([^/]+)";

    server.Get("/api/health", [](const Request&, Response& res) { reply(res, 200, {{"ok", true}, {"service", "vision-line"}}); });
    server.Get("/api/stations", [&stations](const Request&, Response& res) {
        json list = json::array();
        for (const auto& stationId : stations.listStations()) {
            if (auto v = stations.getStation(stationId)) list.push_back(stationJson(v.value()));
        }
        reply(res, 200, {{"stations", std::move(list)}});
    });
    server.Get("/api/stations/" + id, [impl](const Request& req, Response& res) { impl->replyStation(res, req.matches[1]); });
    server.Post("/api/stations/" + id + "/deploy", [impl, &stations](const Request& req, Response& res) {
        const json body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || !body.contains("releaseId") || !body["releaseId"].is_string()) {
            return reply(res, 400, {{"code", "api.bad_request"}, {"message", "请求体里要有文本字段 releaseId"}});
        }
        const std::string stationId = req.matches[1];
        impl->replyStation(res, stationId, stations.deploy(stationId, body["releaseId"].get<std::string>()));
    });
    server.Post("/api/stations/" + id + "/rollback", [impl, &stations](const Request& req, Response& res) {
        const json body = json::parse(req.body, nullptr, false);
        const bool withdraw = body.is_object() && body.value("withdraw", false);
        const std::string stationId = req.matches[1];
        impl->replyStation(res, stationId, stations.rollback(stationId, withdraw));
    });
    server.set_error_handler([](const Request&, Response& res) {
        if (!res.body.empty()) return;
        reply(res, res.status == 200 ? 404 : res.status, {{"code", "api.no_such_endpoint"}, {"message", "没有这个接口"}});
    });
    server.set_exception_handler([](const Request& req, Response& res, std::exception_ptr) {
        log::error("line-api", "处理请求时出错", {{"path", req.path}});
        reply(res, 500, {{"code", "api.internal"}, {"message", "服务内部出错，详情见日志"}});
    });
}

ControlApi::~ControlApi() { stop(); }
int ControlApi::bind(const std::string& host, int port) {
    if (port == 0) return impl_->server.bind_to_any_port(host);
    return impl_->server.bind_to_port(host, port) ? port : -1;
}
void ControlApi::serve() { impl_->server.listen_after_bind(); }
void ControlApi::stop() { impl_->server.stop(); }

// ---------------- 记号文件和转发 ----------------

namespace {

std::filesystem::path markerFile(const std::filesystem::path& dataDir) { return dataDir / "serve.json"; }

// 向正在运行的服务发一个请求。用系统的 curl（和大模型、训练服务用的是同一个客户端）
Result<std::pair<int, json>> call(const std::string& method, const std::string& url, const json& body = nullptr) {
    http::CurlCommandHttpClient client;
    http::HttpRequest request;
    request.method = method;
    request.url = url;
    request.timeoutSeconds = 10;
    if (!body.is_null()) {
        request.headers = {"Content-Type: application/json"};
        request.body = body.dump();
    }
    auto response = client.send(request);
    if (!response) return response.error();
    return std::make_pair(response.value().status, json::parse(response.value().body, nullptr, false));
}

void printStation(const json& v) {
    const auto text = [&](const char* key) { return v.value(key, std::string()); };
    std::cout << "工位 " << text("name") << "  (" << text("id") << ")  ——来自正在运行的服务\n"
              << "  状态          " << text("state") << "\n"
              << "  生效的上线包  " << (text("activeRelease").empty() ? "（无）" : text("activeRelease")) << "\n"
              << "  待切换        "
              << (text("pendingRelease").empty() ? "（无）" : text("pendingRelease") + (v.value("pendingReady", false) ? "（已就绪）" : "（加载中）"))
              << "\n  已检测        " << v.value("sequence", 0) << " 次；连续 ERROR " << v.value("consecutiveErrors", 0) << " 次\n";
}

}  // namespace

void writeServeMarker(const std::filesystem::path& dataDir, int port) {
    std::ofstream(markerFile(dataDir), std::ios::trunc) << json{{"port", port}}.dump();
}

void removeServeMarker(const std::filesystem::path& dataDir) {
    std::error_code ec;
    std::filesystem::remove(markerFile(dataDir), ec);
}

std::string runningService(const std::filesystem::path& dataDir) {
    std::ifstream in(markerFile(dataDir));
    if (!in) return {};
    const json marker = json::parse(in, nullptr, false);
    in.close();
    if (marker.is_object() && marker.contains("port") && marker["port"].is_number_integer()) {
        const std::string url = "http://127.0.0.1:" + std::to_string(marker["port"].get<int>());
        const auto health = call("GET", url + "/api/health");
        if (health && health.value().first == 200 && health.value().second.value("service", "") == "vision-line") return url;
    }
    removeServeMarker(dataDir);  // 上次没有正常退出留下的
    return {};
}

int forwardStationCommand(const std::string& serviceUrl, const std::vector<std::string>& args) {
    const std::string sub = args.size() > 1 ? args[1] : "";
    const auto finish = [](const Result<std::pair<int, json>>& r, const char* done) {
        if (!r) {
            std::cerr << "错误：和正在运行的服务通信失败：" << r.error().message << "\n";
            return 1;
        }
        if (r.value().first != 200) {
            const json& body = r.value().second;
            std::cerr << "错误：" << (body.is_object() ? body.value("message", "服务拒绝了这个操作") : "服务拒绝了这个操作") << "  ("
                      << (body.is_object() ? body.value("code", "") : "") << ")\n";
            return 1;
        }
        if (done != nullptr) std::cout << done << "\n";
        printStation(r.value().second);
        return 0;
    };
    if (sub == "list") {
        const auto r = call("GET", serviceUrl + "/api/stations");
        if (!r || r.value().first != 200) return finish(r, nullptr);
        for (const auto& s : r.value().second.value("stations", json::array())) {
            std::cout << s.value("id", "") << "  " << s.value("name", "") << "  " << s.value("state", "") << "\n";
        }
        return 0;
    }
    if (sub == "show" && args.size() >= 3) return finish(call("GET", serviceUrl + "/api/stations/" + args[2]), nullptr);
    if (sub == "deploy" && args.size() >= 4) {
        return finish(call("POST", serviceUrl + "/api/stations/" + args[2] + "/deploy", {{"releaseId", args[3]}}),
                      "已交给正在运行的服务：上线包在后台加载，加载完成后在下一次检测开始前生效");
    }
    if (sub == "rollback" && args.size() >= 3) {
        const bool withdraw = std::find(args.begin(), args.end(), "--withdraw") != args.end();
        return finish(call("POST", serviceUrl + "/api/stations/" + args[2] + "/rollback", {{"withdraw", withdraw}}),
                      "已交给正在运行的服务：已回滚");
    }
    return -1;
}

}  // namespace vision::line
