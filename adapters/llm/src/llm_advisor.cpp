#include "vision/llm/llm_advisor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>
#include <thread>

namespace vision::llm {

using assistant::AdvisorView;
using assistant::Proposal;
using assistant::ProposedChange;
using nlohmann::json;

namespace {

json paramToJson(const ParamValue& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
    return std::get<std::string>(v);
}

bool judgedCorrectly(const assistant::CaseObservation& o) { return o.verdict == (o.truthNg ? "NG" : "OK"); }

// 发给大模型的用例：判错的全部给；判对的每类最多给几个作参照。用例很多时，这样能把内容控制在合理的长度。
std::vector<const assistant::CaseObservation*> selectCases(const AdvisorView& view) {
    constexpr int kCorrectPerClass = 6;
    std::vector<const assistant::CaseObservation*> out;
    int okShown = 0, ngShown = 0;
    for (const auto& o : view.observations) {
        if (!judgedCorrectly(o)) {
            out.push_back(&o);
        } else if ((o.truthNg ? ngShown : okShown) < kCorrectPerClass) {
            ++(o.truthNg ? ngShown : okShown);
            out.push_back(&o);
        }
    }
    return out;
}

bool retryable(int status) { return status == 408 || status == 409 || status == 429 || status >= 500; }

std::string excerpt(const std::string& s, std::size_t n = 300) { return s.size() <= n ? s : s.substr(0, n) + "……"; }

}  // namespace

// ---------------- 配置 ----------------

Result<LlmConfig> parseLlmConfig(const std::string& text) try {
    using R = Result<LlmConfig>;
    const json j = json::parse(text);
    LlmConfig c;
    c.protocol = j.value("protocol", c.protocol);
    c.baseUrl = j.at("baseUrl").get<std::string>();
    c.model = j.at("model").get<std::string>();
    c.apiKeyEnv = j.value("apiKeyEnv", c.apiKeyEnv);
    c.maxTokens = j.value("maxTokens", c.maxTokens);
    c.timeoutSeconds = j.value("timeoutSeconds", c.timeoutSeconds);
    c.maxRetries = j.value("maxRetries", c.maxRetries);
    c.effort = j.value("effort", c.effort);
    c.maxImages = j.value("maxImages", c.maxImages);
    c.thumbnailSize = j.value("thumbnailSize", c.thumbnailSize);
    if (c.protocol != "anthropic" && c.protocol != "openai") return R::failure("llm.bad_config", "protocol 只能是 anthropic 或 openai");
    if (j.contains("apiKey")) {
        return R::failure("llm.bad_config", "不要把密钥写在配置文件里：用 apiKeyEnv 指定从哪个环境变量读");
    }
    if (c.baseUrl.rfind("https://", 0) != 0 && c.baseUrl.rfind("http://localhost", 0) != 0 &&
        c.baseUrl.rfind("http://127.0.0.1", 0) != 0) {
        return R::failure("llm.bad_config", "baseUrl 必须是 https 地址（本机地址除外）：密钥不能明文在网络上传输");
    }
    return c;
} catch (const std::exception& e) {
    return Result<LlmConfig>::failure("llm.bad_config", std::string("大模型配置文件格式不对：") + e.what());
}

Result<std::unique_ptr<ChatProtocol>> makeProtocol(const LlmConfig& c, const std::string& apiKey) {
    if (apiKey.empty()) {
        return Result<std::unique_ptr<ChatProtocol>>::failure("llm.no_key", "没有密钥：请设置环境变量 " + c.apiKeyEnv);
    }
    ProtocolSettings s{c.baseUrl, c.model, apiKey, c.maxTokens, c.effort};
    if (c.protocol == "anthropic") return makeAnthropicProtocol(std::move(s));
    return makeOpenAiCompatibleProtocol(std::move(s));
}

// ---------------- 提示词 ----------------

std::string LlmAdvisor::systemPrompt() {
    return R"(你是工业视觉检测方案的调参顾问。

一套检测方案由几个图像处理步骤（节点）连成，对每张产品图像给出“OK”（合格）或“NG”（不合格）。方案的结构已经固定，你的工作只是调整其中几个参数（“可调项”），让方案在一组已知答案的图像（“用例”）上判得对。

每一轮你会收到一个 JSON，描述当前的状态：
- goal：要检测什么
- workflow：方案的结构和每个节点的说明
- tunables：可调项，以及各自的允许范围、当前值、说明
- evaluation：当前参数下的评估。misses 是漏检（实际不合格却判成了 OK），false_alarms 是误报（实际合格却判成了 NG），errors 是运行出错
- cases：一部分用例在当前参数下的表现。truth 是正确答案，verdict 是方案给出的判定，outputs 是各节点输出的摘要和统计数字。你看不到图像，这些数字就是你的依据：判断阈值、面积之类的参数应该取多少，要从这里面找
- history：之前每一轮你改了什么、结果如何。kept 表示比之前好，已经保留；rolled-back 表示没有变好，已经撤销（参数回到了之前最好的值）；rejected 表示提议不合规则，没有执行
- rounds_left：还剩几轮

目标：漏检、误报、出错都为 0。达到之后会话会自动结束。漏检比误报严重。

规则：
- 只能修改 tunables 里列出的参数，取值必须在允许范围内。不能增删节点，不能改其他参数；这样的提议会被拒绝，并白白用掉一轮。
- 一轮可以同时修改几个可调项。
- 不要重复 history 里已经试过、没有效果的取值。
- 在能让所有用例判对的取值范围里，尽量选离两边都远的值，不要贴着边界：实际生产的图像会和这些用例略有不同。

只输出一个 JSON 对象，不要输出别的文字，格式：
{"rationale": "一两句话说明你从数据里看到了什么、为什么这样改", "changes": [{"node": "节点的名字", "param": "参数名", "value": 数字}], "finish": false}
如果你认为已经无法再改进，输出 {"rationale": "原因", "changes": [], "finish": true}。)";
}

std::string LlmAdvisor::describe(const AdvisorView& view) {
    json nodes = json::array();
    for (const auto& n : view.nodes) {
        json params = json::object();
        for (const auto& [k, v] : n.params) params[k] = paramToJson(v);
        nodes.push_back({{"name", n.key}, {"operator", n.operatorId}, {"fixed_params", params}});
    }
    json tunables = json::array();
    for (const auto& t : view.tunables) {
        tunables.push_back({{"node", t.tunable.nodeKey}, {"param", t.tunable.param}, {"min", t.tunable.min},
                            {"max", t.tunable.max}, {"current", t.current}, {"hint", t.tunable.hint}});
    }
    json evaluation = nullptr;
    if (view.best) {
        evaluation = {{"cases", view.best->total()}, {"ok_cases", view.best->truthOk}, {"ng_cases", view.best->truthNg},
                      {"misses", view.best->misses}, {"false_alarms", view.best->falseAlarms}, {"errors", view.best->errors},
                      {"wrong_cases", view.best->wrongCaseIds}};
    }
    json history = json::array();
    for (const auto& r : view.history) {
        json changes = json::array();
        for (const auto& c : r.changes) changes.push_back({{"node", c.nodeKey}, {"param", c.param}, {"value", c.value}});
        json entry = {{"round", r.index}, {"changes", changes}};
        if (r.outcome == "rejected") {
            entry["result"] = "rejected";
            entry["reason"] = r.rejectReason;
        } else {
            entry["result"] = r.decision;
            entry["misses"] = r.misses;
            entry["false_alarms"] = r.falseAlarms;
            entry["errors"] = r.errors;
        }
        history.push_back(std::move(entry));
    }
    json cases = json::array();
    for (const auto* o : selectCases(view)) {
        cases.push_back({{"id", o->caseId}, {"truth", o->truthNg ? "NG" : "OK"}, {"verdict", o->verdict},
                         {"correct", judgedCorrectly(*o)}, {"reasons", o->reasons}, {"outputs", o->outputs}});
    }
    const json j = {{"goal", view.goal},
                    {"workflow", {{"name", view.workflowName}, {"description", view.workflowDescription}, {"nodes", nodes}}},
                    {"tunables", tunables},
                    {"evaluation", evaluation},
                    {"cases", cases},
                    {"cases_note", "判错的用例全部列出；判对的每类最多列 6 个作参照"},
                    {"history", history},
                    {"rounds_left", view.roundsLeft}};
    return j.dump(1);
}

// ---------------- 解析回复 ----------------

Proposal LlmAdvisor::parseProposal(const std::string& reply) {
    Proposal unreadable;  // 空的提议：助手会以“提议里没有任何修改”拒绝它，循环继续
    unreadable.rationale = "（大模型的回复无法解析）" + excerpt(reply, 200);

    // 回复应该正好是一个 JSON 对象；有的模型会在前后加说明或 ``` 标记，所以取第一个 { 到最后一个 } 之间的部分
    const auto begin = reply.find('{');
    const auto end = reply.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end < begin) return unreadable;
    const json j = json::parse(reply.substr(begin, end - begin + 1), nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return unreadable;

    Proposal p;
    if (j.contains("rationale") && j["rationale"].is_string()) p.rationale = j["rationale"].get<std::string>();
    if (j.contains("finish") && j["finish"].is_boolean()) p.finish = j["finish"].get<bool>();
    if (j.contains("changes") && j["changes"].is_array()) {
        for (const auto& jc : j["changes"]) {
            if (!jc.is_object()) return unreadable;
            ProposedChange c;
            // 大模型可能自作主张提出改变结构的修改：如实转成对应的类型，由助手的规则去拒绝，而不是在这里悄悄丢掉
            const std::string kind = jc.contains("kind") && jc["kind"].is_string() ? jc["kind"].get<std::string>() : "set-param";
            if (kind == "add-node") c.kind = ProposedChange::Kind::AddNode;
            else if (kind == "remove-node") c.kind = ProposedChange::Kind::RemoveNode;
            else if (kind == "connect") c.kind = ProposedChange::Kind::Connect;
            else if (kind == "disconnect") c.kind = ProposedChange::Kind::Disconnect;
            else if (kind == "set-roi") c.kind = ProposedChange::Kind::SetRoi;
            if (jc.contains("node") && jc["node"].is_string()) c.nodeKey = jc["node"].get<std::string>();
            if (jc.contains("param") && jc["param"].is_string()) c.param = jc["param"].get<std::string>();
            if (jc.contains("value")) {
                const json& v = jc["value"];
                if (v.is_number()) {
                    c.value = v.get<double>();
                } else if (v.is_string()) {  // "195" 这样写成字符串的数字也接受
                    try {
                        c.value = std::stod(v.get<std::string>());
                    } catch (const std::exception&) {
                        c.value = std::nan("");  // 不是数字：助手会以“超出范围”拒绝
                    }
                } else {
                    c.value = std::nan("");
                }
            } else if (c.kind == ProposedChange::Kind::SetParam) {
                c.value = std::nan("");
            }
            p.changes.push_back(std::move(c));
        }
    }
    if (!p.finish && p.changes.empty() && p.rationale.empty()) return unreadable;
    return p;
}

// ---------------- 顾问 ----------------

LlmAdvisor::LlmAdvisor(LlmConfig config, std::unique_ptr<ChatProtocol> protocol, HttpClient& http, ImageLoader images)
    : config_(std::move(config)), protocol_(std::move(protocol)), http_(http), images_(std::move(images)) {}

std::string LlmAdvisor::name() const { return "llm:" + protocol_->name() + ":" + config_.model; }

Result<Proposal> LlmAdvisor::propose(const AdvisorView& view) {
    using R = Result<Proposal>;
    ChatRequest chat;
    chat.system = systemPrompt();
    chat.user = describe(view);

    // 图像：只有数据外发策略允许、并且助手确实把图像的引用给了我们，才会附上。
    // “不外发图像”时助手给的 imageRef 是空的（A9），这里想发也无从发起。
    if (images_ && view.dataSharing != assistant::DataSharing::NoImages) {
        std::vector<const assistant::CaseObservation*> order;
        for (const auto& o : view.observations) {
            if (!judgedCorrectly(o)) order.push_back(&o);  // 判错的优先
        }
        for (const auto& o : view.observations) {
            if (judgedCorrectly(o)) order.push_back(&o);
        }
        for (const auto* o : order) {
            if (static_cast<int>(chat.images.size()) >= config_.maxImages) break;
            if (o->imageRef.empty()) continue;
            auto image = images_(o->imageRef, o->thumbnailOnly ? config_.thumbnailSize : 0);
            if (!image) continue;  // 图像读不出来不算致命：这一幅不发
            image.value().caption = "用例 " + o->caseId + "（正确答案 " + (o->truthNg ? "NG" : "OK") + "，方案判定 " + o->verdict + "）的图像：";
            chat.images.push_back(std::move(image).value());
        }
    }

    const HttpRequest request = protocol_->buildRequest(chat, config_.timeoutSeconds);
    LlmCall call;
    call.requestBody = request.body;
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&](R result) {
        call.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (!result) call.error = result.error().message;
        calls_.push_back(std::move(call));
        return result;
    };

    std::string lastError;
    for (int attempt = 0; attempt <= config_.maxRetries; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 1 ? 1000 : 3000));
        call.attempts = attempt + 1;
        auto response = http_.post(request);
        if (!response) {  // 没有收到回复：可能是暂时的，再试
            lastError = response.error().message;
            continue;
        }
        call.httpStatus = response.value().status;
        if (response.value().status != 200) {
            lastError = "大模型服务返回 " + std::to_string(response.value().status) + "：" + excerpt(response.value().body);
            if (retryable(response.value().status)) continue;  // 限流、服务繁忙：再试
            return finish(R::failure("llm.http_error", lastError));  // 密钥不对、请求不合法……再试也没用
        }
        auto reply = protocol_->parseReply(response.value().body);
        if (!reply) return finish(reply.error());
        call.replyText = reply.value().text;
        call.inputTokens = reply.value().inputTokens;
        call.outputTokens = reply.value().outputTokens;
        return finish(parseProposal(reply.value().text));
    }
    return finish(R::failure("llm.unavailable", "大模型服务不可用（试了 " + std::to_string(config_.maxRetries + 1) + " 次）：" + lastError));
}

}  // namespace vision::llm
