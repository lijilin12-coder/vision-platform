// 大模型顾问的测试（POC-12）。全部用假的 HTTP 客户端：不访问网络，不需要密钥。
#include <gtest/gtest.h>

#include <deque>
#include <nlohmann/json.hpp>

#include "vision/llm/chat_protocol.h"
#include "vision/llm/http_client.h"
#include "vision/llm/llm_advisor.h"

using namespace vision;
using namespace vision::llm;
using assistant::AdvisorView;
using assistant::Proposal;
using assistant::ProposedChange;
using nlohmann::json;

namespace {

// 假的 HTTP 客户端：按顺序给出事先准备好的回复，并记下收到的每一个请求
class FakeHttp final : public HttpClient {
public:
    Result<HttpResponse> send(const HttpRequest& request) override {
        requests.push_back(request);
        if (replies.empty()) return Result<HttpResponse>::failure("http.unreachable", "模拟：断网");
        auto next = replies.front();
        replies.pop_front();
        return next;
    }
    void reply(int status, const std::string& body) { replies.emplace_back(HttpResponse{status, body}); }
    void fail() { replies.emplace_back(Error{"http.unreachable", "模拟：超时"}); }

    std::deque<Result<HttpResponse>> replies;
    std::vector<HttpRequest> requests;
};

std::string openAiReply(const std::string& content) {
    return json{{"model", "fake-model"},
                {"choices", json::array({{{"finish_reason", "stop"}, {"message", {{"role", "assistant"}, {"content", content}}}}})},
                {"usage", {{"prompt_tokens", 100}, {"completion_tokens", 20}}}}
        .dump();
}

LlmConfig fastConfig() {
    LlmConfig c;
    c.baseUrl = "https://llm.example";
    c.model = "fake-model";
    c.maxRetries = 1;
    return c;
}

AdvisorView sampleView() {
    AdvisorView v;
    v.goal = "检出亮斑";
    v.workflowName = "亮斑检测";
    v.nodes = {{"bin", "vision.threshold", {{"method", std::string("fixed")}}}};
    v.tunables = {{{"bin", "value", 0, 255, 100, "阈值"}, 100, 100}};
    assistant::Evaluation e;
    e.caseIds = {"ok1", "ng1"};
    e.truthOk = 1;
    e.truthNg = 1;
    e.falseAlarms = 1;
    e.wrongCaseIds = {"ok1"};
    v.best = e;
    assistant::CaseObservation ok;
    ok.caseId = "ok1";
    ok.verdict = "NG";
    ok.outputs["gray.image"] = "image 640x480；灰度 最小 60，p50 150，最大 162";
    assistant::CaseObservation ng;
    ng.caseId = "ng1";
    ng.truthNg = true;
    ng.verdict = "NG";
    v.observations = {ok, ng};
    v.roundsLeft = 5;
    return v;
}

LlmAdvisor makeAdvisor(FakeHttp& http, ImageLoader images = {}) {
    return LlmAdvisor(fastConfig(), makeOpenAiCompatibleProtocol({"https://llm.example", "fake-model", "KEY", 1000, ""}), http,
                      std::move(images));
}

}  // namespace

// ---------------- 两种协议 ----------------

TEST(ChatProtocol, OpenAiCompatibleRequestAndReply) {
    const auto p = makeOpenAiCompatibleProtocol({"https://api.deepseek.com/", "deepseek-chat", "sk-test", 4000, ""});
    const HttpRequest r = p->buildRequest({"系统", "用户", {}}, 60);
    EXPECT_EQ(r.url, "https://api.deepseek.com/chat/completions");
    EXPECT_NE(std::find(r.headers.begin(), r.headers.end(), "Authorization: Bearer sk-test"), r.headers.end());
    const json body = json::parse(r.body);
    EXPECT_EQ(body["model"], "deepseek-chat");
    EXPECT_EQ(body["messages"][0]["role"], "system");
    EXPECT_EQ(body["messages"][1]["content"], "用户");
    EXPECT_EQ(body["response_format"]["type"], "json_object");
    EXPECT_EQ(r.body.find("sk-test"), std::string::npos);  // 密钥只在请求头里，不在请求体里

    auto reply = p->parseReply(openAiReply("{\"finish\": true}"));
    ASSERT_TRUE(reply.ok());
    EXPECT_EQ(reply.value().text, "{\"finish\": true}");
    EXPECT_EQ(reply.value().inputTokens, 100);

    EXPECT_EQ(p->parseReply(R"({"choices":[{"finish_reason":"length","message":{"content":"{"}}]})").error().code, "llm.truncated");
    EXPECT_EQ(p->parseReply("<html>网关错误</html>").error().code, "llm.bad_reply");
}

TEST(ChatProtocol, AnthropicRequestAndReply) {
    const auto p = makeAnthropicProtocol({"https://api.anthropic.com", "claude-opus-5-5", "sk-ant-test", 16000, "high"});
    const HttpRequest r = p->buildRequest({"系统", "用户", {{"image/jpeg", "QUJD", "用例 a 的图像："}}}, 60);
    EXPECT_EQ(r.url, "https://api.anthropic.com/v1/messages");
    EXPECT_NE(std::find(r.headers.begin(), r.headers.end(), "x-api-key: sk-ant-test"), r.headers.end());
    EXPECT_NE(std::find(r.headers.begin(), r.headers.end(), "anthropic-version: 2026-10-21"), r.headers.end());
    const json body = json::parse(r.body);
    EXPECT_EQ(body["model"], "claude-opus-5-5");
    EXPECT_EQ(body["system"], "系统");
    EXPECT_EQ(body["output_config"]["effort"], "high");
    EXPECT_FALSE(body.contains("thinking"));     // 不发 thinking 参数
    EXPECT_FALSE(body.contains("temperature"));  // 新的模型不接受采样参数
    const json& content = body["messages"][0]["content"];
    ASSERT_EQ(content.size(), 3u);  // 图像的说明、图像、正文
    EXPECT_EQ(content[1]["type"], "image");
    EXPECT_EQ(content[1]["source"]["data"], "QUJD");
    EXPECT_EQ(content[2]["text"], "用户");

    // 回复里可能有思考块：只取文字块
    auto reply = p->parseReply(R"({"model":"claude-opus-5-5","stop_reason":"end_turn",
        "content":[{"type":"thinking","thinking":""},{"type":"text","text":"{\"finish\": true}"}],
        "usage":{"input_tokens":50,"output_tokens":9}})");
    ASSERT_TRUE(reply.ok());
    EXPECT_EQ(reply.value().text, "{\"finish\": true}");
    EXPECT_EQ(reply.value().outputTokens, 9);

    // 先看结束原因，再读内容
    EXPECT_EQ(p->parseReply(R"({"stop_reason":"refusal","content":[]})").error().code, "llm.refusal");
    EXPECT_EQ(p->parseReply(R"({"stop_reason":"max_tokens","content":[{"type":"text","text":"{"}]})").error().code, "llm.truncated");
}

// ---------------- 解析大模型的回复 ----------------

TEST(LlmAdvisor, ParsesAProposal) {
    const Proposal p = LlmAdvisor::parseProposal(
        R"({"rationale": "背景最亮 162", "changes": [{"node": "bin", "param": "value", "value": 195}], "finish": false})");
    EXPECT_EQ(p.rationale, "背景最亮 162");
    ASSERT_EQ(p.changes.size(), 1u);
    EXPECT_EQ(p.changes[0].kind, ProposedChange::Kind::SetParam);
    EXPECT_EQ(p.changes[0].nodeKey, "bin");
    EXPECT_EQ(p.changes[0].value, 195);
    EXPECT_FALSE(p.finish);
}

TEST(LlmAdvisor, ToleratesWrappingTextAndStringNumbers) {
    const Proposal p = LlmAdvisor::parseProposal(
        "好的，我的建议如下：\n```json\n{\"rationale\": \"x\", \"changes\": [{\"node\": \"bin\", \"param\": \"value\", \"value\": \"180\"}]}\n```");
    ASSERT_EQ(p.changes.size(), 1u);
    EXPECT_EQ(p.changes[0].value, 180);
    EXPECT_TRUE(LlmAdvisor::parseProposal(R"({"rationale": "够了", "changes": [], "finish": true})").finish);
}

// 看不懂的回复变成一份空的提议（助手会拒绝它，循环继续），而不是抛异常或者让程序崩溃
TEST(LlmAdvisor, UnreadableReplyBecomesAnEmptyProposal) {
    for (const char* reply : {"", "我认为应该提高阈值。", "{ 这不是 JSON }", "[1, 2, 3]", R"({"changes": "all"})", R"({"changes": [5]})", "{}"}) {
        const Proposal p = LlmAdvisor::parseProposal(reply);
        EXPECT_TRUE(p.changes.empty()) << reply;
        EXPECT_FALSE(p.finish) << reply;
    }
}

// 大模型自作主张的修改不在这里悄悄丢掉：如实交给助手的规则去拒绝
TEST(LlmAdvisor, KeepsOutOfBoundsChangesForTheRulesToReject) {
    const Proposal p = LlmAdvisor::parseProposal(
        R"({"rationale": "x", "changes": [{"kind": "add-node", "node": "blur"}, {"node": "judge", "param": "maxCount", "value": 9},
            {"node": "bin", "param": "value", "value": "很大"}]})");
    ASSERT_EQ(p.changes.size(), 3u);
    EXPECT_EQ(p.changes[0].kind, ProposedChange::Kind::AddNode);
    EXPECT_EQ(p.changes[1].nodeKey, "judge");
    EXPECT_TRUE(std::isnan(p.changes[2].value));  // 不是数字：范围检查通不过
}

// ---------------- 发给大模型的内容 ----------------

TEST(LlmAdvisor, DescribesTheStateForTheModel) {
    const json j = json::parse(LlmAdvisor::describe(sampleView()));
    EXPECT_EQ(j["goal"], "检出亮斑");
    EXPECT_EQ(j["tunables"][0]["node"], "bin");
    EXPECT_EQ(j["tunables"][0]["max"], 255);
    EXPECT_EQ(j["evaluation"]["false_alarms"], 1);
    EXPECT_EQ(j["rounds_left"], 5);
    ASSERT_EQ(j["cases"].size(), 2u);
    EXPECT_EQ(j["cases"][0]["truth"], "OK");
    EXPECT_EQ(j["cases"][0]["correct"], false);
    EXPECT_NE(j["cases"][0]["outputs"]["gray.image"].get<std::string>().find("p50 150"), std::string::npos);
    EXPECT_NE(LlmAdvisor::systemPrompt().find("只输出一个 JSON 对象"), std::string::npos);
}

// 用例很多时不全发：判错的全部发，判对的每类最多 6 个
TEST(LlmAdvisor, LimitsHowManyCorrectCasesAreSent) {
    AdvisorView v = sampleView();
    v.observations.clear();
    for (int i = 0; i < 50; ++i) {
        assistant::CaseObservation o;
        o.caseId = "ok" + std::to_string(i);
        o.verdict = i < 3 ? "NG" : "OK";  // 前 3 个判错
        v.observations.push_back(o);
    }
    const json j = json::parse(LlmAdvisor::describe(v));
    EXPECT_EQ(j["cases"].size(), 9u);  // 3 个判错的 + 6 个判对的
}

// ---------------- 调用、重试、失败 ----------------

TEST(LlmAdvisor, ProposesFromTheModelsReply) {
    FakeHttp http;
    http.reply(200, openAiReply(R"({"rationale": "r", "changes": [{"node": "bin", "param": "value", "value": 195}]})"));
    LlmAdvisor advisor = makeAdvisor(http);
    auto p = advisor.propose(sampleView());
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(p.value().changes[0].value, 195);
    EXPECT_EQ(advisor.name(), "llm:openai:fake-model");
    ASSERT_EQ(advisor.calls().size(), 1u);
    EXPECT_EQ(advisor.calls()[0].attempts, 1);
    EXPECT_EQ(advisor.calls()[0].inputTokens, 100);
    EXPECT_EQ(advisor.calls()[0].requestBody.find("KEY"), std::string::npos);  // 记录里没有密钥
}

TEST(LlmAdvisor, RetriesWhenTheServiceIsBusy) {
    FakeHttp http;
    http.reply(429, R"({"error": "rate limited"})");
    http.reply(200, openAiReply(R"({"rationale": "r", "changes": [{"node": "bin", "param": "value", "value": 190}]})"));
    LlmAdvisor advisor = makeAdvisor(http);
    auto p = advisor.propose(sampleView());
    ASSERT_TRUE(p.ok());
    EXPECT_EQ(http.requests.size(), 2u);
    EXPECT_EQ(advisor.calls()[0].attempts, 2);
}

TEST(LlmAdvisor, DoesNotRetryWhenTheKeyIsWrong) {
    FakeHttp http;
    http.reply(401, R"({"error": "invalid api key"})");
    http.reply(200, openAiReply("{}"));
    LlmAdvisor advisor = makeAdvisor(http);
    auto p = advisor.propose(sampleView());
    ASSERT_FALSE(p.ok());
    EXPECT_EQ(p.error().code, "llm.http_error");
    EXPECT_EQ(http.requests.size(), 1u);  // 再试也没用
}

TEST(LlmAdvisor, FailsWhenTheServiceStaysUnavailable) {
    FakeHttp http;  // 没有准备任何回复：每次都是“断网”
    LlmAdvisor advisor = makeAdvisor(http);
    auto p = advisor.propose(sampleView());
    ASSERT_FALSE(p.ok());
    EXPECT_EQ(p.error().code, "llm.unavailable");
    EXPECT_EQ(http.requests.size(), 2u);  // 第一次 + 重试 1 次
    EXPECT_FALSE(advisor.calls()[0].error.empty());
}

// ---------------- 数据外发（A9）----------------

TEST(LlmAdvisor, SendsNoImagesUnlessThePolicyAllowsAndRefsAreGiven) {
    std::vector<std::pair<std::string, int>> loaded;
    const ImageLoader loader = [&loaded](const std::string& ref, int maxSide) -> Result<ChatImage> {
        loaded.emplace_back(ref, maxSide);
        return ChatImage{"image/jpeg", "QUJD", ""};
    };
    const std::string good = openAiReply(R"({"rationale": "r", "changes": [], "finish": true})");

    {  // 不外发图像：助手给的内容里没有图像的引用，顾问想发也发不了
        FakeHttp http;
        http.reply(200, good);
        LlmAdvisor advisor = makeAdvisor(http, loader);
        AdvisorView v = sampleView();  // dataSharing 默认是 NoImages，imageRef 是空的
        ASSERT_TRUE(advisor.propose(v).ok());
        EXPECT_TRUE(loaded.empty());
        EXPECT_EQ(http.requests[0].body.find("image_url"), std::string::npos);
    }
    {  // 只外发缩略图：图像按缩略图的尺寸读
        FakeHttp http;
        http.reply(200, good);
        LlmAdvisor advisor = makeAdvisor(http, loader);
        AdvisorView v = sampleView();
        v.dataSharing = assistant::DataSharing::Thumbnails;
        for (auto& o : v.observations) {
            o.imageRef = "/img/" + o.caseId + ".png";
            o.thumbnailOnly = true;
        }
        ASSERT_TRUE(advisor.propose(v).ok());
        ASSERT_EQ(loaded.size(), 2u);
        EXPECT_EQ(loaded[0].first, "/img/ok1.png");  // 判错的用例排在前面
        EXPECT_EQ(loaded[0].second, 256);
        EXPECT_NE(http.requests[0].body.find("image_url"), std::string::npos);
        loaded.clear();
    }
    {  // 外发原图：不缩小
        FakeHttp http;
        http.reply(200, good);
        LlmAdvisor advisor = makeAdvisor(http, loader);
        AdvisorView v = sampleView();
        v.dataSharing = assistant::DataSharing::Originals;
        for (auto& o : v.observations) o.imageRef = "/img/" + o.caseId + ".png";
        ASSERT_TRUE(advisor.propose(v).ok());
        ASSERT_EQ(loaded.size(), 2u);
        EXPECT_EQ(loaded[0].second, 0);
    }
}

// ---------------- 配置 ----------------

TEST(LlmConfig, ParsesAndRejectsUnsafeSettings) {
    auto c = parseLlmConfig(R"({"protocol": "openai", "baseUrl": "https://api.deepseek.com", "model": "deepseek-chat"})");
    ASSERT_TRUE(c.ok());
    EXPECT_EQ(c.value().apiKeyEnv, "VISION_LLM_API_KEY");

    // 密钥不许写在配置文件里（配置文件会进代码仓库）
    EXPECT_FALSE(parseLlmConfig(R"({"baseUrl": "https://a.b", "model": "m", "apiKey": "sk-123"})").ok());
    // 不许用明文 http 发密钥
    EXPECT_FALSE(parseLlmConfig(R"({"baseUrl": "http://a.b", "model": "m"})").ok());
    EXPECT_TRUE(parseLlmConfig(R"({"baseUrl": "http://localhost:8080", "model": "m"})").ok());
    EXPECT_FALSE(parseLlmConfig(R"({"protocol": "other", "baseUrl": "https://a.b", "model": "m"})").ok());
    EXPECT_FALSE(parseLlmConfig("不是 JSON").ok());

    EXPECT_EQ(makeProtocol(c.value(), "").error().code, "llm.no_key");
    EXPECT_TRUE(makeProtocol(c.value(), "sk-x").ok());
}

// ---------------- 真正的 HTTP 客户端 ----------------

// 连不上的地址：必须返回失败，不能卡住，也不能把临时文件（里面有密钥）留在磁盘上
TEST(CurlCommandHttpClient, ReportsFailureWhenTheHostIsUnreachable) {
    CurlCommandHttpClient http;
    HttpRequest request;
    request.url = "https://127.0.0.1:1/v1/chat";  // 本机一个没有人监听的端口：不依赖网络
    request.headers = {"Authorization: Bearer secret-for-test"};
    request.body = "{}";
    request.timeoutSeconds = 5;
    const auto response = http.send(request);
    ASSERT_FALSE(response.ok());
    EXPECT_EQ(response.error().message.find("secret-for-test"), std::string::npos);  // 出错信息里没有密钥
}
