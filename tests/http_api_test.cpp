// vision-runtime 的 HTTP 接口（POC-2 第 ① 步）。
// 测试里启动一个真的服务（监听本机一个空闲端口），用 HTTP 客户端访问它。
//
// 要验证的不是业务规则本身（那些在领域层的测试里），而是“接口层只做翻译”：
//   - 规则不允许的操作被拒绝，而且原因和直接调用应用服务时一字不差
//   - 通过接口搭的方案，和通过应用服务（命令行走的路）搭的是同一个东西
//   - 同时来很多请求也不出错

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <opencv2/imgproc.hpp>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "http_api.h"
#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"

using namespace vision;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Reply {
    int status = 0;
    json body;
    std::string raw, contentType;
};

class HttpApi : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        // 中文数据目录
        dir = fs::temp_directory_path() / pathFromUtf8(std::string("vision-接口-") + info->name());
        fs::remove_all(dir);
        platform = std::make_unique<platform::Platform>(dir);
        server = std::make_unique<runtime::ApiServer>(*platform);
        port = server->bind("127.0.0.1", 0);
        ASSERT_GT(port, 0);
        serving = std::thread([this] { server->serve(); });
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
        // 等服务真的开始应答
        for (int i = 0; i < 200 && !client->Get("/api/health"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    void TearDown() override {
        server->stop();
        if (serving.joinable()) serving.join();
        client.reset();
        server.reset();
        platform.reset();
        fs::remove_all(dir);
    }

    static Reply toReply(const httplib::Result& r) {
        Reply reply;
        EXPECT_TRUE(static_cast<bool>(r)) << "请求没有得到回复";
        if (!r) return reply;
        reply.status = r->status;
        reply.raw = r->body;
        reply.contentType = r->get_header_value("Content-Type");
        reply.body = json::parse(r->body, nullptr, false);
        return reply;
    }
    Reply get(const std::string& path) { return toReply(client->Get(path)); }
    Reply post(const std::string& path, const json& body = json::object()) {
        return toReply(client->Post(path, body.dump(), "application/json"));
    }
    Reply put(const std::string& path, const json& body) { return toReply(client->Put(path, body.dump(), "application/json")); }
    Reply del(const std::string& path, const json& body = json::object()) {
        return toReply(client->Delete(path, body.dump(), "application/json"));
    }

    // 深色零件上有 spots 个亮斑的图像，编码成 PNG
    static std::string png(int spots, int width = 640, int height = 480) {
        cv::Mat m(height, width, CV_8UC1, cv::Scalar(80));
        for (int i = 0; i < spots; ++i) cv::circle(m, {100 + 120 * i, height / 2}, 10, cv::Scalar(240), cv::FILLED);
        const auto bytes = imaging::encodePng(imaging::fromMat(m).value()).value();
        return std::string(bytes.begin(), bytes.end());
    }
    std::string upload(const std::string& content) {
        const auto r = toReply(client->Post("/api/images", content, "image/png"));
        EXPECT_EQ(r.status, 201) << r.raw;
        return r.body.value("imageId", "");
    }

    // 通过接口搭出“输入 → 灰度 → 阈值 → Blob → 个数判定”。返回方案 ID；节点依次是 n1～n5
    std::string brightSpotRecipe(const std::string& name = "亮斑") {
        const auto created = post("/api/recipes", {{"name", name}});
        EXPECT_EQ(created.status, 201) << created.raw;
        const std::string id = created.body.value("id", "");
        const std::string base = "/api/recipes/" + id;
        for (const char* op : {"vision.image_input", "vision.to_gray", "vision.threshold", "vision.blob", "vision.judge_count"}) {
            EXPECT_EQ(post(base + "/nodes", {{"operatorId", op}}).status, 201);
        }
        for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
                 {"n1.image", "n2.image"}, {"n2.image", "n3.image"}, {"n3.region", "n4.region"}, {"n4.blobs", "n5.blobs"}}) {
            EXPECT_EQ(post(base + "/edges", {{"from", from}, {"to", to}}).status, 200);
        }
        EXPECT_EQ(put(base + "/nodes/n3/params/value", {{"value", 200}}).status, 200);
        return id;
    }

    fs::path dir;
    std::unique_ptr<platform::Platform> platform;
    std::unique_ptr<runtime::ApiServer> server;
    std::unique_ptr<httplib::Client> client;
    std::thread serving;
    int port = 0;
};

}  // namespace

TEST_F(HttpApi, OperatorsCarryEverythingNeededToBuildAParameterPanel) {
    const auto r = get("/api/operators");
    ASSERT_EQ(r.status, 200);
    EXPECT_NE(r.contentType.find("application/json"), std::string::npos);
    const auto& list = r.body["operators"];
    EXPECT_EQ(list.size(), catalog::OperatorCatalog::global().list().size());
    json threshold;
    for (const auto& op : list) {
        if (op["id"] == "vision.threshold") threshold = op;
    }
    ASSERT_FALSE(threshold.is_null());
    EXPECT_EQ(threshold["inputs"][0]["type"], "image");
    EXPECT_EQ(threshold["outputs"][0]["type"], "region");
    EXPECT_FALSE(threshold["displayName"].get<std::string>().empty());
    // 参数：类型、默认值、范围、选项、说明都在
    json value, method;
    for (const auto& p : threshold["params"]) {
        if (p["name"] == "value") value = p;
        if (p["name"] == "method") method = p;
    }
    EXPECT_EQ(value["type"], "number");
    EXPECT_TRUE(value["default"].is_number());
    EXPECT_EQ(value["max"], 255);
    EXPECT_EQ(method["type"], "choice");
    EXPECT_GE(method["choices"].size(), 2u);
    EXPECT_FALSE(value["label"].get<std::string>().empty());
}

// 通过标准 1、2：搭方案、上传图像、运行、看每个节点的输出
TEST_F(HttpApi, BuildRunAndLookAtIntermediateResults) {
    const std::string recipe = brightSpotRecipe();
    const auto shown = get("/api/recipes/" + recipe);
    ASSERT_EQ(shown.status, 200);
    EXPECT_EQ(shown.body["name"], "亮斑");
    EXPECT_EQ(shown.body["nodes"].size(), 5u);
    EXPECT_EQ(shown.body["edges"].size(), 4u);
    EXPECT_EQ(shown.body["nodes"][2]["params"]["value"], 200);

    const std::string image = upload(png(3));
    EXPECT_EQ(image.size(), 64u);
    EXPECT_EQ(upload(png(3)), image);  // 同一幅图像再传一次：还是同一个 ID

    const auto run = post("/api/recipes/" + recipe + "/debug-runs", {{"imageId", image}});
    ASSERT_EQ(run.status, 201) << run.raw;
    EXPECT_EQ(run.body["verdict"], "NG");
    ASSERT_EQ(run.body["reasons"].size(), 1u);
    EXPECT_FALSE(run.body["reasons"][0]["pass"].get<bool>());
    ASSERT_EQ(run.body["nodes"].size(), 5u);
    for (const auto& n : run.body["nodes"]) EXPECT_EQ(n["status"], "succeeded");
    // 结果项：3 个缺陷，类别是判定节点的 label，位置就是亮斑的位置
    const auto& defects = run.body["result"]["defects"];
    ASSERT_EQ(defects.size(), 3u);
    EXPECT_EQ(defects[0]["nodeId"], "n5");
    EXPECT_FALSE(defects[0]["category"].get<std::string>().empty());
    EXPECT_NEAR(defects[0]["centerY"].get<double>(), 240.0, 1.0);
    EXPECT_GT(defects[0]["area"].get<double>(), 100);
    EXPECT_TRUE(run.body["result"]["measurements"].empty());
    // 数字、连通块的值直接在结果里
    const auto& blobNode = run.body["nodes"][3];
    EXPECT_EQ(blobNode["outputs"]["count"]["value"], 3);
    EXPECT_EQ(blobNode["outputs"]["blobs"]["value"].size(), 3u);
    EXPECT_GT(blobNode["outputs"]["blobs"]["value"][0]["area"].get<double>(), 100);
    EXPECT_EQ(run.body["nodes"][4]["outputs"]["judgement"]["value"]["pass"], false);
    // 调试运行带统计信息（给人或方案助手看参数该怎么调）
    EXPECT_FALSE(run.body["nodes"][1]["outputs"]["image"]["statistics"].get<std::string>().empty());

    // 图像和区域：按地址取 PNG，解码出来尺寸对、内容对
    const auto region = client->Get(run.body["nodes"][2]["outputs"]["region"]["imageUrl"].get<std::string>());
    ASSERT_TRUE(region);
    ASSERT_EQ(region->status, 200);
    EXPECT_EQ(region->get_header_value("Content-Type"), "image/png");
    const auto mask = imaging::decodeImage(std::vector<unsigned char>(region->body.begin(), region->body.end()));
    ASSERT_TRUE(mask.ok());
    const cv::Mat m = imaging::view(mask.value());
    EXPECT_EQ(m.size(), cv::Size(640, 480));
    EXPECT_EQ(m.at<uchar>(240, 100), 255);  // 第一个亮斑的中心
    EXPECT_EQ(m.at<uchar>(10, 10), 0);
    const auto gray = client->Get(run.body["nodes"][1]["outputs"]["image"]["imageUrl"].get<std::string>());
    ASSERT_TRUE(gray);
    EXPECT_EQ(gray->status, 200);

    // 合格的图像
    const auto ok = post("/api/recipes/" + recipe + "/debug-runs", {{"imageId", upload(png(0))}});
    EXPECT_EQ(ok.body["verdict"], "OK");
}

// 通过标准 4：规则在领域层。接口报的原因和直接调用应用服务时一字不差
TEST_F(HttpApi, RuleViolationsAreRejectedWithTheServicesOwnWords) {
    const std::string recipe = brightSpotRecipe();
    const std::string base = "/api/recipes/" + recipe;
    auto& authoring = platform->authoring;

    // 类型不对的连线
    auto r = post(base + "/edges", {{"from", "n1.image"}, {"to", "n4.region"}});
    auto direct = authoring.connect(recipe, "n1.image", "n4.region");
    ASSERT_FALSE(direct.ok());
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.body["code"], direct.error().code);
    EXPECT_EQ(r.body["message"], direct.error().message);

    // 超出范围的参数；类型不对的参数
    r = put(base + "/nodes/n3/params/value", {{"value", 999}});
    direct = authoring.setParam(recipe, "n3", "value", 999.0);
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.body["message"], direct.error().message);
    EXPECT_EQ(put(base + "/nodes/n3/params/value", {{"value", "很亮"}}).status, 422);
    EXPECT_EQ(put(base + "/nodes/n3/params/nope", {{"value", 1}}).status, 404);

    // 会形成环的连线（阈值的输出没法接回灰度，换一对类型相同的：灰度 → 灰度自己）
    EXPECT_EQ(post(base + "/edges", {{"from", "n2.image"}, {"to", "n2.image"}}).status, 422);

    // 被拒绝的操作不留痕迹
    EXPECT_EQ(get(base).body["edges"].size(), 4u);
    EXPECT_EQ(get(base).body["nodes"][2]["params"]["value"], 200);

    // 缺判定就发布
    const auto empty = post("/api/recipes", {{"name", "空的"}}).body["id"].get<std::string>();
    r = post("/api/recipes/" + empty + "/publish");
    const auto directPublish = authoring.publish(empty);
    EXPECT_EQ(r.status, 422);
    EXPECT_EQ(r.body["message"], directPublish.error().message);

    // 找不到的东西
    EXPECT_EQ(get("/api/recipes/nobody").status, 404);
    EXPECT_EQ(post("/api/recipes/nobody/nodes", {{"operatorId", "vision.to_gray"}}).status, 404);
    EXPECT_EQ(del(base + "/nodes/n99").status, 404);
    EXPECT_EQ(get("/api/nothing-here").status, 404);
    EXPECT_EQ(get("/api/nothing-here").body["code"], "api.no_such_endpoint");
}

// 通过标准 5：接口和命令行（应用服务）操作的是同一个东西
TEST_F(HttpApi, WhatTheApiBuildsIsWhatTheServicesSeeAndViceVersa) {
    // 接口搭的 → 应用服务看得到，发布出来的版本能直接检测
    const std::string recipe = brightSpotRecipe();
    const auto draft = platform->authoring.getDraft(recipe);
    ASSERT_TRUE(draft.ok());
    EXPECT_EQ(draft.value().nodes.size(), 5u);
    const auto published = post("/api/recipes/" + recipe + "/publish");
    ASSERT_EQ(published.status, 201) << published.raw;
    const auto doc = platform->authoring.getVersionDocument(published.body["versionId"].get<std::string>());
    ASSERT_TRUE(doc.ok());
    const std::string three = png(3);
    const auto image = imaging::decodeImage(std::vector<unsigned char>(three.begin(), three.end())).value();
    EXPECT_EQ(platform->execution.inspect(doc.value(), image).value().verdict, "NG");

    // 应用服务搭的 → 接口看得到，而且能接着改
    const auto other = platform->authoring.createRecipe("命令行搭的").value();
    const auto input = platform->authoring.addNode(other, "vision.image_input").value();
    const auto toGray = platform->authoring.addNode(other, "vision.to_gray").value();
    ASSERT_TRUE(platform->authoring.connect(other, input + ".image", toGray + ".image").ok());
    ASSERT_TRUE(platform->authoring.setRoi(other, toGray, Roi::rect(10, 20, 300, 200).value()).ok());
    const auto seen = get("/api/recipes/" + other);
    ASSERT_EQ(seen.status, 200);
    EXPECT_EQ(seen.body["nodes"].size(), 2u);
    EXPECT_EQ(seen.body["nodes"][1]["roi"]["width"], 300);
    const auto list = get("/api/recipes");
    EXPECT_EQ(list.body["recipes"].size(), 2u);

    // 接着改：清除 ROI、删连线、删节点（删节点同时删掉它的连线，R6）
    EXPECT_TRUE(del("/api/recipes/" + other + "/nodes/" + toGray + "/roi").body["nodes"][1]["roi"].is_null());
    EXPECT_EQ(put("/api/recipes/" + other + "/nodes/" + toGray + "/roi", {{"x", 0}, {"y", 0}, {"width", 50}, {"height", 40}}).status, 200);
    EXPECT_EQ(put("/api/recipes/" + other + "/nodes/" + toGray + "/roi", {{"x", 0}, {"y", 0}, {"width", -5}, {"height", 40}}).status, 422);
    EXPECT_EQ(del("/api/recipes/" + other + "/edges", {{"from", input + ".image"}, {"to", toGray + ".image"}}).body["edges"].size(), 0u);
    EXPECT_EQ(post("/api/recipes/" + other + "/edges", {{"from", input + ".image"}, {"to", toGray + ".image"}}).status, 200);
    const auto removed = del("/api/recipes/" + other + "/nodes/" + toGray);
    EXPECT_EQ(removed.body["nodes"].size(), 1u);
    EXPECT_EQ(removed.body["edges"].size(), 0u);
    EXPECT_EQ(platform->authoring.getDraft(other).value().nodes.size(), 1u);
}

TEST_F(HttpApi, MalformedRequestsGetAReadable400AndNeverCrashTheServer) {
    const std::string recipe = brightSpotRecipe();
    const std::string base = "/api/recipes/" + recipe;
    const auto raw = [this](const std::string& path, const std::string& body) {
        return toReply(client->Post(path, body, "application/json"));
    };
    EXPECT_EQ(raw("/api/recipes", "这不是 JSON").status, 400);
    EXPECT_EQ(raw("/api/recipes", "[1, 2]").status, 400);
    EXPECT_EQ(raw("/api/recipes", "{}").status, 400);              // 没有 name
    EXPECT_EQ(raw("/api/recipes", R"({"name": 7})").status, 400);  // name 不是文本
    EXPECT_EQ(raw(base + "/nodes", "{}").status, 400);
    EXPECT_EQ(raw(base + "/edges", R"({"from": "n1.image"})").status, 400);
    EXPECT_EQ(raw(base + "/edges", R"({"from": "没有点号", "to": "n2.image"})").status, 422);
    EXPECT_EQ(put(base + "/nodes/n3/params/value", json::object()).status, 400);
    EXPECT_EQ(put(base + "/nodes/n3/params/value", {{"value", {1, 2}}}).status, 400);
    EXPECT_EQ(put(base + "/nodes/n3/roi", {{"x", 1}}).status, 400);
    EXPECT_EQ(raw("/api/recipes", std::string("{\"name\": \"\xff\xfe\"}")).status, 400);  // 不是合法的 UTF-8
    EXPECT_EQ(raw(base + "/debug-runs", "{}").status, 400);

    // 图像
    EXPECT_EQ(toReply(client->Post("/api/images", "这不是图像", "image/png")).status, 422);
    EXPECT_EQ(toReply(client->Post("/api/images", "", "image/png")).status, 422);
    EXPECT_EQ(post(base + "/debug-runs", {{"imageId", "0000"}}).status, 404);
    EXPECT_EQ(post(base + "/debug-runs", {{"imageId", "../../etc/passwd"}}).status, 404);  // 图像 ID 不能被当成路径

    // 服务还活着
    EXPECT_EQ(get("/api/health").status, 200);
    EXPECT_EQ(get(base).body["nodes"].size(), 5u);
}

TEST_F(HttpApi, DebugOutputsAreKeptOnlyForTheLastFewRuns) {
    const std::string recipe = brightSpotRecipe();
    const std::string image = upload(png(1));
    const auto first = post("/api/recipes/" + recipe + "/debug-runs", {{"imageId", image}});
    const std::string firstUrl = first.body["nodes"][1]["outputs"]["image"]["imageUrl"];
    EXPECT_EQ(client->Get(firstUrl)->status, 200);
    // 不是图像的输出、不存在的输出
    const std::string runBase = "/api/debug-runs/" + first.body["runId"].get<std::string>() + "/outputs/";
    EXPECT_EQ(get(runBase + "n4/count").status, 422);
    EXPECT_EQ(get(runBase + "n4/nope").status, 404);
    EXPECT_EQ(get(runBase + "n99/image").status, 404);

    std::string lastUrl;
    for (int i = 0; i < 8; ++i) {
        lastUrl = post("/api/recipes/" + recipe + "/debug-runs", {{"imageId", image}}).body["nodes"][1]["outputs"]["image"]["imageUrl"];
    }
    const auto gone = get(firstUrl);  // 第一次的结果已经被挤掉了
    EXPECT_EQ(gone.status, 404);
    EXPECT_EQ(gone.body["code"], "api.run_not_found");
    EXPECT_EQ(client->Get(lastUrl)->status, 200);
}

// 模型参数和节点失败：没选模型的异常检测节点失败，上游照常有输出（界面要能显示“哪一步出了问题”）
TEST_F(HttpApi, FailedNodesAreReportedWithoutLosingUpstreamOutputs) {
    const auto recipe = post("/api/recipes", {{"name", "模型"}}).body["id"].get<std::string>();
    const std::string base = "/api/recipes/" + recipe;
    for (const char* op : {"vision.image_input", "vision.to_gray", "vision.anomaly_detect"}) post(base + "/nodes", {{"operatorId", op}});
    post(base + "/edges", {{"from", "n1.image"}, {"to", "n2.image"}});
    post(base + "/edges", {{"from", "n2.image"}, {"to", "n3.image"}});
    const auto run = post(base + "/debug-runs", {{"imageId", upload(png(1))}});
    ASSERT_EQ(run.status, 201) << run.raw;
    EXPECT_EQ(run.body["verdict"], "ERROR");
    EXPECT_EQ(run.body["nodes"][1]["status"], "succeeded");
    EXPECT_TRUE(run.body["nodes"][1]["outputs"].contains("image"));
    EXPECT_EQ(run.body["nodes"][2]["status"], "failed");
    EXPECT_NE(run.body["nodes"][2]["message"].get<std::string>().find("还没有设置"), std::string::npos);
}

// 通过标准 8：同时来很多请求。几个线程各自搭自己的方案、反复运行；另一个线程不停地读。
// 这个测试在 ThreadSanitizer 下也要干净。
TEST_F(HttpApi, ManyRequestsAtOnce) {
    const std::string image = upload(png(2));
    const std::string shared = brightSpotRecipe("大家都读的");
    std::atomic<int> failures{0};
    std::atomic<bool> stop{false};
    const auto check = [&failures](bool ok) {
        if (!ok) ++failures;
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            httplib::Client c("127.0.0.1", port);  // 每个线程自己的连接
            const auto send = [&c](const char* method, const std::string& path, const json& body) {
                const std::string m = method;
                auto r = m == "PUT" ? c.Put(path, body.dump(), "application/json") : c.Post(path, body.dump(), "application/json");
                return r ? std::make_pair(r->status, json::parse(r->body, nullptr, false)) : std::make_pair(0, json());
            };
            auto [status, created] = send("POST", "/api/recipes", {{"name", "并发 " + std::to_string(t)}});
            check(status == 201);
            if (status != 201) return;
            const std::string base = "/api/recipes/" + created["id"].get<std::string>();
            for (const char* op : {"vision.image_input", "vision.to_gray", "vision.threshold", "vision.blob", "vision.judge_count"}) {
                check(send("POST", base + "/nodes", {{"operatorId", op}}).first == 201);
            }
            for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
                     {"n1.image", "n2.image"}, {"n2.image", "n3.image"}, {"n3.region", "n4.region"}, {"n4.blobs", "n5.blobs"}}) {
                check(send("POST", base + "/edges", {{"from", from}, {"to", to}}).first == 200);
            }
            for (int i = 0; i < 6; ++i) {
                check(send("PUT", base + "/nodes/n3/params/value", {{"value", 150 + i * 10}}).first == 200);
                auto [runStatus, run] = send("POST", base + "/debug-runs", {{"imageId", image}});
                check(runStatus == 201 && run["verdict"] == "NG" && run["nodes"][3]["outputs"]["count"]["value"] == 2);
                // 图像可能已经被别的线程的运行挤出缓存：404 也是正确的回答，500 或没有回复才是错
                if (runStatus == 201) {
                    const auto img = c.Get(run["nodes"][2]["outputs"]["region"]["imageUrl"].get<std::string>());
                    check(img && (img->status == 200 || img->status == 404));
                }
            }
        });
    }
    std::thread reader([&] {
        httplib::Client c("127.0.0.1", port);
        while (!stop) {
            const auto a = c.Get("/api/recipes"), b = c.Get("/api/recipes/" + shared), o = c.Get("/api/operators");
            check(a && a->status == 200 && b && b->status == 200 && o && o->status == 200);
        }
    });
    for (auto& t : threads) t.join();
    stop = true;
    reader.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(get("/api/recipes").body["recipes"].size(), 5u);
}

// 工位页面要用的接口：上线包、工位、持续运行、热切换。
// 流程：发布两个方案版本（严格的、宽松的）→ 各做成上线包 → 建工位、绑定图像目录、部署 → 开始 → 运行中换上线包
TEST_F(HttpApi, StationsRunContinuouslyAndHotSwapThroughTheApi) {
    const auto eventually = [](const std::function<bool()>& condition) {
        for (int i = 0; i < 400; ++i) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return false;
    };
    const auto publishWith = [&](int maxCount) {
        const std::string recipe = brightSpotRecipe("亮斑 ≤ " + std::to_string(maxCount));
        EXPECT_EQ(put("/api/recipes/" + recipe + "/nodes/n5/params/maxCount", {{"value", maxCount}}).status, 200);
        const auto published = post("/api/recipes/" + recipe + "/publish");
        EXPECT_EQ(published.status, 201);
        const auto release = post("/api/releases", {{"recipeVersionId", published.body.value("versionId", "")}});
        EXPECT_EQ(release.status, 201) << release.raw;
        return release.body.value("releaseId", std::string());
    };
    const std::string strict = publishWith(0), lenient = publishWith(5);
    const auto releases = get("/api/releases");
    ASSERT_EQ(releases.body["releases"].size(), 2u);
    EXPECT_FALSE(releases.body["releases"][0]["recipeName"].get<std::string>().empty());
    EXPECT_EQ(post("/api/releases", {{"recipeVersionId", "nobody@v1"}}).status, 404);

    // 图像目录（中文名）
    const fs::path images = dir / pathFromUtf8("来料");
    fs::create_directories(images);
    const auto writeImage = [&](const std::string& name, int spots) {
        const std::string content = png(spots);
        std::ofstream(images / name, std::ios::binary) << content;
    };
    writeImage("1.png", 2);

    const auto created = post("/api/stations", {{"name", "一号工位"}});
    ASSERT_EQ(created.status, 200) << created.raw;
    const std::string station = created.body["id"];
    const std::string base = "/api/stations/" + station;
    EXPECT_EQ(created.body["state"], "stopped");
    EXPECT_FALSE(created.body["runner"]["running"].get<bool>());

    // 还没有图像源、没有上线包：开始不了，原因说清楚
    EXPECT_EQ(post(base + "/start").body["code"], "station.no_image_source");
    EXPECT_EQ(put(base + "/image-source", {{"kind", "folder"}, {"location", pathToUtf8(images / "nope")}}).status, 404);
    ASSERT_EQ(put(base + "/image-source", {{"kind", "folder"}, {"location", pathToUtf8(images)}}).status, 200);
    EXPECT_EQ(post(base + "/start").status, 422);  // 没有上线包
    EXPECT_EQ(get(base + "/frame").status, 404);    // 还没有检测过

    ASSERT_EQ(post(base + "/deploy", {{"releaseId", strict}}).status, 200);
    ASSERT_TRUE(eventually([&] { return get(base).body.value("pendingReady", false); }));
    const auto started = post(base + "/start");
    ASSERT_EQ(started.status, 200) << started.raw;
    EXPECT_TRUE(started.body["runner"]["running"].get<bool>());
    EXPECT_EQ(post(base + "/start").status, 200);  // 已经在跑：再点一次不出错

    // 第一幅：严格的方案，2 个亮斑 → NG
    ASSERT_TRUE(eventually([&] { return get(base).body["runner"]["ng"] == 1; }));
    auto status = get(base);
    ASSERT_EQ(status.body["recent"].size(), 1u);
    EXPECT_EQ(status.body["recent"][0]["frameName"], "1.png");
    EXPECT_EQ(status.body["recent"][0]["verdict"], "NG");
    EXPECT_EQ(status.body["recent"][0]["result"]["defects"].size(), 2u);
    EXPECT_EQ(status.body["activeRelease"], strict);
    ASSERT_TRUE(eventually([&] { return get(base).body["runner"]["idle"].get<bool>(); }));
    // 最近的那一帧能取回来
    const auto frame = client->Get(base + "/frame");
    ASSERT_TRUE(frame && frame->status == 200);
    EXPECT_EQ(frame->get_header_value("Content-Type"), "image/png");
    // 运行中不能换图像源（S7）
    EXPECT_EQ(put(base + "/image-source", {{"kind", "folder"}, {"location", pathToUtf8(images)}}).body["code"], "station.source_while_running");

    // 运行中部署宽松的方案；下一幅图像到来时切换。同样 2 个亮斑，这次是 OK
    ASSERT_EQ(post(base + "/deploy", {{"releaseId", lenient}}).status, 200);
    ASSERT_TRUE(eventually([&] { return get(base).body.value("pendingReady", false); }));
    writeImage("2.png", 2);
    ASSERT_TRUE(eventually([&] { return get(base).body["runner"]["ok"] == 1; }));
    // ?after=1：只要第一次之后的记录
    status = get(base + "?after=1");
    ASSERT_EQ(status.body["recent"].size(), 1u);
    EXPECT_EQ(status.body["recent"][0]["frameName"], "2.png");
    EXPECT_TRUE(status.body["recent"][0]["swapped"].get<bool>());
    EXPECT_EQ(status.body["activeRelease"], lenient);
    EXPECT_EQ(get(base + "?after=abc").status, 400);

    // 保存下来的检测记录：从新到旧；不合格的那一次带着当时的图像
    auto records = get(base + "/records");
    ASSERT_EQ(records.status, 200) << records.raw;
    ASSERT_EQ(records.body["records"].size(), 2u);
    EXPECT_EQ(records.body["records"][0]["frameName"], "2.png");
    EXPECT_EQ(records.body["records"][0]["imageId"], "");
    const auto ngRecord = records.body["records"][1];
    EXPECT_EQ(ngRecord["verdict"], "NG");
    EXPECT_EQ(ngRecord["releaseId"], strict);
    EXPECT_EQ(ngRecord["result"]["defects"].size(), 2u);
    EXPECT_EQ(ngRecord["time"].get<std::string>().size(), 19u);
    const auto kept = client->Get("/api/images/" + ngRecord["imageId"].get<std::string>());
    ASSERT_TRUE(kept && kept->status == 200);
    EXPECT_EQ(kept->get_header_value("Content-Type"), "image/png");
    // 筛选、翻页
    records = get(base + "/records?verdict=NG");
    ASSERT_EQ(records.body["records"].size(), 1u);
    EXPECT_EQ(records.body["records"][0]["id"], ngRecord["id"]);
    EXPECT_EQ(get(base + "/records?limit=1").body["records"].size(), 1u);
    EXPECT_TRUE(get(base + "/records?before=" + ngRecord["id"].get<std::string>()).body["records"].empty());
    EXPECT_EQ(get(base + "/records?verdict=MAYBE").status, 400);
    EXPECT_EQ(get(base + "/records?limit=abc").status, 400);
    EXPECT_EQ(get("/api/stations/nobody/records").status, 404);
    const auto summary = get(base + "/summary");
    ASSERT_EQ(summary.status, 200);
    EXPECT_EQ(summary.body["total"], 2);
    EXPECT_EQ(summary.body["ng"], 1);
    EXPECT_DOUBLE_EQ(summary.body["yield"].get<double>(), 0.5);
    EXPECT_EQ(summary.body["defectsByCategory"].size(), 1u);
    EXPECT_EQ(get("/api/stations/nobody/summary").status, 404);

    // 回滚；工位列表里看得到运行情况，但不带检测记录
    ASSERT_EQ(post(base + "/rollback").status, 200);
    EXPECT_EQ(get(base).body["activeRelease"], strict);
    const auto list = get("/api/stations");
    ASSERT_EQ(list.body["stations"].size(), 1u);
    EXPECT_TRUE(list.body["stations"][0]["runner"]["running"].get<bool>());
    EXPECT_TRUE(list.body["stations"][0]["recent"].empty());

    // 停止
    const auto stopped = post(base + "/stop");
    ASSERT_EQ(stopped.status, 200);
    EXPECT_FALSE(stopped.body["runner"]["running"].get<bool>());
    EXPECT_EQ(stopped.body["state"], "stopped");
    EXPECT_EQ(post(base + "/stop").status, 200);  // 没在跑：再停一次不出错
    EXPECT_EQ(get("/api/stations/nobody").status, 404);
}

// 验收检查：用两个目录做基准图集 → 给上线包验收 → 没通过的部署不了
TEST_F(HttpApi, AcceptanceCheckThroughTheApi) {
    const auto writeTo = [&](const fs::path& folder, const std::string& name, int spots, int width) {
        fs::create_directories(folder);
        const std::string content = png(spots, width);
        std::ofstream(folder / name, std::ios::binary) << content;
    };
    const fs::path okDir = dir / pathFromUtf8("基准") / "ok", ngDir = dir / pathFromUtf8("基准") / "ng";
    writeTo(okDir, "1.png", 0, 640);
    writeTo(okDir, "2.png", 0, 600);  // 尺寸不同：内容不同，不是同一个样本
    writeTo(ngDir, "1.png", 1, 640);
    writeTo(ngDir, "2.png", 3, 640);

    EXPECT_TRUE(get("/api/golden-sets").body["goldenSets"].empty());
    const auto created = post("/api/golden-sets", {{"name", "亮斑基准"}, {"okDirectory", pathToUtf8(okDir)}, {"ngDirectory", pathToUtf8(ngDir)}});
    ASSERT_EQ(created.status, 201) << created.raw;
    EXPECT_EQ(created.body["cases"], 4);
    EXPECT_EQ(created.body["truthNg"], 2);
    EXPECT_EQ(created.body["criteria"]["maxMisses"], 0);
    const std::string set = created.body["id"];
    EXPECT_EQ(get("/api/golden-sets").body["goldenSets"].size(), 1u);
    EXPECT_EQ(post("/api/golden-sets", {{"name", "x"}, {"okDirectory", pathToUtf8(dir / "nope")}}).status, 404);
    EXPECT_EQ(post("/api/golden-sets", {{"okDirectory", pathToUtf8(okDir)}}).status, 400);  // 没有名称
    EXPECT_EQ(post("/api/golden-sets", {{"name", "x"}, {"okDirectory", 5}}).status, 400);

    const auto releaseWith = [&](int maxCount) {
        const std::string recipe = brightSpotRecipe("亮斑 ≤ " + std::to_string(maxCount));
        EXPECT_EQ(put("/api/recipes/" + recipe + "/nodes/n5/params/maxCount", {{"value", maxCount}}).status, 200);
        const auto published = post("/api/recipes/" + recipe + "/publish");
        return post("/api/releases", {{"recipeVersionId", published.body.value("versionId", "")}}).body.value("releaseId", std::string());
    };
    const std::string strict = releaseWith(0), lenient = releaseWith(2);
    const auto find = [&](const std::string& releaseId) {
        const auto listed = get("/api/releases");
        for (const auto& r : listed.body["releases"]) {
            if (r["id"] == releaseId) return r;
        }
        return json();
    };
    EXPECT_EQ(find(strict)["acceptanceState"], "none");
    EXPECT_TRUE(find(strict)["acceptance"].is_null());

    auto checked = post("/api/releases/" + strict + "/acceptance", {{"goldenSetId", set}});
    ASSERT_EQ(checked.status, 200) << checked.raw;
    EXPECT_TRUE(checked.body["passed"].get<bool>());
    EXPECT_EQ(checked.body["total"], 4);
    EXPECT_TRUE(checked.body["wrongCases"].empty());
    EXPECT_EQ(find(strict)["acceptanceState"], "passed");
    EXPECT_EQ(find(strict)["acceptance"]["goldenSetName"], "亮斑基准");

    checked = post("/api/releases/" + lenient + "/acceptance", {{"goldenSetId", set}});
    ASSERT_EQ(checked.status, 200) << checked.raw;  // 检查做完了，结论是不行：不是出错
    EXPECT_FALSE(checked.body["passed"].get<bool>());
    EXPECT_EQ(checked.body["misses"], 1);
    ASSERT_EQ(checked.body["wrongCases"].size(), 1u);
    EXPECT_EQ(checked.body["wrongCases"][0]["caseId"], "ng/1.png");
    EXPECT_EQ(checked.body["wrongCases"][0]["kind"], "miss");
    EXPECT_EQ(find(lenient)["acceptanceState"], "failed");

    EXPECT_EQ(post("/api/releases/" + strict + "/acceptance", {{"goldenSetId", "nobody"}}).status, 404);
    EXPECT_EQ(post("/api/releases/nobody/acceptance", {{"goldenSetId", set}}).status, 404);
    EXPECT_EQ(post("/api/releases/" + strict + "/acceptance").status, 400);

    // 没通过的部署不了；通过的可以
    const std::string station = post("/api/stations", {{"name", "一号工位"}}).body["id"];
    const auto refused = post("/api/stations/" + station + "/deploy", {{"releaseId", lenient}});
    EXPECT_EQ(refused.status, 422);
    EXPECT_EQ(refused.body["code"], "station.release_not_accepted");
    EXPECT_EQ(post("/api/stations/" + station + "/deploy", {{"releaseId", strict}}).status, 200);
}

// 通过标准 3（服务一侧）：改一个参数 → 重新运行 → 取回一幅中间结果，一共要多久。
// 这里只打印数字、设一个很宽的上限（测试机的快慢差别很大）；正式的数字记在结论报告里。
TEST_F(HttpApi, ParameterChangeToPreviewRoundTripIsFast) {
    const std::string recipe = brightSpotRecipe();
    const std::string base = "/api/recipes/" + recipe;
    for (const auto& [width, height] : std::vector<std::pair<int, int>>{{640, 480}, {2592, 1944}}) {
        const std::string image = upload(png(3, width, height));
        double best = 1e9;
        for (int i = 0; i < 5; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            ASSERT_EQ(put(base + "/nodes/n3/params/value", {{"value", 180 + i}}).status, 200);
            const auto run = post(base + "/debug-runs", {{"imageId", image}});
            ASSERT_EQ(run.status, 201);
            const auto img = client->Get(run.body["nodes"][2]["outputs"]["region"]["imageUrl"].get<std::string>());
            ASSERT_TRUE(img && img->status == 200);
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::cout << "[ 耗时 ] " << width << "x" << height << "：改参数 + 调试运行 + 取一幅区域图 = " << best << " ms（5 次里最快的）\n";
        if (width == 640) {
            EXPECT_LT(best, 3000.0);
        }
    }
}
