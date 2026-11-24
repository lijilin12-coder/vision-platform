// 训练后端的 HTTP 适配器（POC-13 第 3 步）。
// 这里不启动真正的训练服务（CI 的机器上没有 PyTorch）：用假的 HTTP 客户端检查
// “发出去的请求对不对”和“各种回复有没有翻译对”。训练服务自己的测试在 services/training/test_service.py。

#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "vision/training_http/http_training_backend.h"

using namespace vision;
using nlohmann::json;
using training::BackendProgress;

namespace {

class FakeHttp final : public http::HttpClient {
public:
    Result<http::HttpResponse> send(const http::HttpRequest& request) override {
        requests.push_back(request);
        if (replies.empty()) return Result<http::HttpResponse>::failure("http.unreachable", "模拟：连接被拒绝");
        const auto reply = replies.front();
        replies.pop_front();
        return reply;
    }
    void reply(int status, const std::string& body) { replies.push_back({status, body}); }

    std::vector<http::HttpRequest> requests;
    std::deque<http::HttpResponse> replies;
};

training::TrainingRequest sampleRequest() {
    training::TrainingRequest r;
    r.jobId = "job-1";
    r.config = {"anomaly-autoencoder", "surface", {{"epochs", 40}, {"imageSize", 256}}};
    r.samples = {{"aaa", false, training::Split::Train, "data/content/a.png"},
                 {"bbb", true, training::Split::Test, "data/content/b.png"},
                 {"ccc", false, training::Split::Validation, "data/content/c.png"}};
    return r;
}

}  // namespace

TEST(HttpTrainingBackend, StartPostsConfigAndSamplesWithAbsolutePaths) {
    FakeHttp http;
    http.reply(200, R"({"ticket":"t-42"})");
    training_http::HttpTrainingBackend backend(http, "http://127.0.0.1:8765/");  // 末尾的斜杠不碍事

    const auto ticket = backend.start(sampleRequest());
    ASSERT_TRUE(ticket) << ticket.error().message;
    EXPECT_EQ(ticket.value(), "t-42");

    ASSERT_EQ(http.requests.size(), 1u);
    EXPECT_EQ(http.requests[0].method, "POST");
    EXPECT_EQ(http.requests[0].url, "http://127.0.0.1:8765/jobs");
    const json body = json::parse(http.requests[0].body);
    EXPECT_EQ(body["jobId"], "job-1");
    EXPECT_EQ(body["config"]["modelKind"], "anomaly-autoencoder");
    EXPECT_EQ(body["config"]["settings"]["imageSize"], 256);
    ASSERT_EQ(body["samples"].size(), 3u);
    EXPECT_EQ(body["samples"][0]["split"], "train");
    EXPECT_EQ(body["samples"][1]["split"], "test");
    EXPECT_EQ(body["samples"][1]["ng"], true);
    EXPECT_EQ(body["samples"][2]["split"], "val");
    // 训练服务的当前目录和平台的不一样：路径必须是绝对的
    EXPECT_TRUE(std::filesystem::path(body["samples"][0]["path"].get<std::string>()).is_absolute());
}

TEST(HttpTrainingBackend, StartFailuresAreTold) {
    FakeHttp http;
    training_http::HttpTrainingBackend backend(http, "http://127.0.0.1:8765");
    // 没有回复：连不上
    auto r = backend.start(sampleRequest());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "training.backend_unreachable");
    // 训练服务拒绝，并说明了原因
    http.reply(400, R"({"error":"这个训练服务只会训练 anomaly-autoencoder"})");
    r = backend.start(sampleRequest());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "training.backend_rejected");
    EXPECT_NE(r.error().message.find("只会训练"), std::string::npos);
    // 回复看不懂
    for (const char* body : {"<html>502</html>", "{}", R"({"ticket":7})", "[1,2]"}) {
        http.reply(200, body);
        r = backend.start(sampleRequest());
        ASSERT_FALSE(r) << body;
        EXPECT_EQ(r.error().code, "training.backend_bad_reply");
    }
}

TEST(HttpTrainingBackend, PollTranslatesRunningCompletedAndFailed) {
    FakeHttp http;
    training_http::HttpTrainingBackend backend(http, "http://127.0.0.1:8765");

    http.reply(200, R"({"state":"running","progress":0.4,"failure":""})");
    auto p = backend.poll("t-1");
    ASSERT_TRUE(p);
    EXPECT_EQ(p.value().state, BackendProgress::State::Running);
    EXPECT_DOUBLE_EQ(p.value().progress, 0.4);
    EXPECT_EQ(http.requests.back().method, "GET");
    EXPECT_EQ(http.requests.back().url, "http://127.0.0.1:8765/jobs/t-1");
    EXPECT_TRUE(http.requests.back().body.empty());

    http.reply(200, R"({"state":"completed","progress":1.0,
        "report":{"metrics":{"test_auroc":0.98,"threshold":0.008},"seconds":9.5,"notes":"备注"},
        "output":{"modelFile":"/work/t-1/model.onnx","contractKind":"anomaly-autoencoder","inputWidth":256,"inputHeight":256,
                  "inputChannels":1,"outputMeaning":"anomaly-score-and-map","numbers":{"threshold":0.008}}})");
    p = backend.poll("t-1");
    ASSERT_TRUE(p) << p.error().message;
    EXPECT_EQ(p.value().state, BackendProgress::State::Completed);
    EXPECT_DOUBLE_EQ(p.value().report.metrics.at("test_auroc"), 0.98);
    EXPECT_DOUBLE_EQ(p.value().report.seconds, 9.5);
    EXPECT_EQ(p.value().report.notes, "备注");
    EXPECT_EQ(p.value().output.modelFile, "/work/t-1/model.onnx");
    EXPECT_EQ(p.value().output.inputWidth, 256);
    EXPECT_EQ(p.value().output.contractKind, "anomaly-autoencoder");
    EXPECT_DOUBLE_EQ(p.value().output.numbers.at("threshold"), 0.008);

    http.reply(200, R"({"state":"failed","progress":0.2,"failure":"训练集里合格的图像只有 2 幅，太少了"})");
    p = backend.poll("t-1");
    ASSERT_TRUE(p);
    EXPECT_EQ(p.value().state, BackendProgress::State::Failed);
    EXPECT_NE(p.value().failure.find("太少"), std::string::npos);
}

TEST(HttpTrainingBackend, PollDistinguishesLostJobFromTemporaryTrouble) {
    FakeHttp http;
    training_http::HttpTrainingBackend backend(http, "http://127.0.0.1:8765");
    // 404：训练服务重启过，这次训练已经没有了——这是确定的失败
    http.reply(404, R"({"error":"没有这次训练"})");
    auto p = backend.poll("t-1");
    ASSERT_TRUE(p);
    EXPECT_EQ(p.value().state, BackendProgress::State::Failed);
    // 连不上：暂时查不到，任务不应该因此被判失败
    p = backend.poll("t-1");
    ASSERT_FALSE(p);
    EXPECT_EQ(p.error().code, "training.backend_unreachable");
    // 看不懂的回复：同样只是“这次没查到”
    for (const char* body : {"not json", R"({"state":"dancing"})", R"({"state":"completed"})", R"({"state":"completed","report":3,"output":"x"})",
                             R"({"state":"completed","output":{"modelFile":""}})"}) {
        http.reply(200, body);
        p = backend.poll("t-1");
        ASSERT_FALSE(p) << body;
        EXPECT_EQ(p.error().code, "training.backend_bad_reply");
    }
    http.reply(500, "oops");
    EXPECT_FALSE(backend.poll("t-1"));
}

TEST(HttpTrainingBackend, CancelSendsDeleteAndToleratesForgottenJobs) {
    FakeHttp http;
    training_http::HttpTrainingBackend backend(http, "http://127.0.0.1:8765");
    http.reply(200, R"({"ok":true})");
    EXPECT_TRUE(backend.cancel("t-9"));
    EXPECT_EQ(http.requests.back().method, "DELETE");
    EXPECT_EQ(http.requests.back().url, "http://127.0.0.1:8765/jobs/t-9");
    http.reply(404, "{}");
    EXPECT_TRUE(backend.cancel("t-9"));
    http.reply(500, "{}");
    EXPECT_FALSE(backend.cancel("t-9"));
    EXPECT_FALSE(backend.cancel("t-9"));  // 连不上
}
