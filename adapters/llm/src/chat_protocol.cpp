#include "vision/llm/chat_protocol.h"

#include <nlohmann/json.hpp>

namespace vision::llm {

using nlohmann::json;

namespace {

std::string joinUrl(const std::string& base, const std::string& path) {
    return (!base.empty() && base.back() == '/' ? base.substr(0, base.size() - 1) : base) + path;
}

// ---------- Anthropic（Claude）：POST /v1/messages ----------
class AnthropicProtocol final : public ChatProtocol {
public:
    explicit AnthropicProtocol(ProtocolSettings s) : s_(std::move(s)) {}
    std::string name() const override { return "anthropic"; }

    HttpRequest buildRequest(const ChatRequest& chat, int timeoutSeconds) const override {
        json content = json::array();
        for (const auto& image : chat.images) {
            if (!image.caption.empty()) content.push_back({{"type", "text"}, {"text", image.caption}});
            content.push_back({{"type", "image"},
                               {"source", {{"type", "base64"}, {"media_type", image.mediaType}, {"data", image.base64}}}});
        }
        content.push_back({{"type", "text"}, {"text", chat.user}});

        json body = {{"model", s_.model},
                     {"max_tokens", s_.maxTokens},
                     {"system", chat.system},
                     {"messages", json::array({{{"role", "user"}, {"content", content}}})},
                     // 模型出于安全策略拒绝回答时，由服务端换一个模型重试，而不是让这一轮白白失败
                     {"fallbacks", "default"}};
        // 思考的深度由 effort 控制；不发 thinking 参数（新的模型上思考总是开着的）
        if (!s_.effort.empty()) body["output_config"] = {{"effort", s_.effort}};

        HttpRequest r;
        r.url = joinUrl(s_.baseUrl, "/v1/messages");
        r.headers = {"Content-Type: application/json", "x-api-key: " + s_.apiKey, "anthropic-version: 2026-10-21",
                     "anthropic-beta: server-side-fallback-2026-07-01"};
        r.body = body.dump();
        r.timeoutSeconds = timeoutSeconds;
        return r;
    }

    Result<ChatReply> parseReply(const std::string& body) const override try {
        using R = Result<ChatReply>;
        const json j = json::parse(body);
        const std::string stop = j.value("stop_reason", "");
        // 先看结束原因，再读内容
        if (stop == "refusal") return R::failure("llm.refusal", "大模型拒绝回答这个请求");
        if (stop == "max_tokens") return R::failure("llm.truncated", "大模型的回复被截断了（max_tokens 太小）");
        ChatReply reply;
        for (const auto& block : j.at("content")) {
            if (block.value("type", "") == "text") reply.text += block.value("text", "");  // 思考块等其他类型跳过
        }
        if (j.contains("usage")) {
            reply.inputTokens = j["usage"].value("input_tokens", 0);
            reply.outputTokens = j["usage"].value("output_tokens", 0);
        }
        reply.model = j.value("model", "");
        return reply;
    } catch (const std::exception& e) {
        return Result<ChatReply>::failure("llm.bad_reply", std::string("看不懂大模型服务的回复：") + e.what());
    }

private:
    ProtocolSettings s_;
};

// ---------- OpenAI 兼容：POST /chat/completions ----------
class OpenAiCompatibleProtocol final : public ChatProtocol {
public:
    explicit OpenAiCompatibleProtocol(ProtocolSettings s) : s_(std::move(s)) {}
    std::string name() const override { return "openai"; }

    HttpRequest buildRequest(const ChatRequest& chat, int timeoutSeconds) const override {
        json user;
        if (chat.images.empty()) {
            user = chat.user;  // 没有图像时用最简单的形式：不支持图像的模型也能接受
        } else {
            user = json::array();
            for (const auto& image : chat.images) {
                if (!image.caption.empty()) user.push_back({{"type", "text"}, {"text", image.caption}});
                user.push_back({{"type", "image_url"},
                                {"image_url", {{"url", "data:" + image.mediaType + ";base64," + image.base64}}}});
            }
            user.push_back({{"type", "text"}, {"text", chat.user}});
        }
        const json body = {{"model", s_.model},
                           {"max_tokens", s_.maxTokens},
                           {"messages", json::array({{{"role", "system"}, {"content", chat.system}},
                                                     {{"role", "user"}, {"content", user}}})},
                           {"response_format", {{"type", "json_object"}}}};  // 要求回复是一个 JSON 对象
        HttpRequest r;
        r.url = joinUrl(s_.baseUrl, "/chat/completions");
        r.headers = {"Content-Type: application/json", "Authorization: Bearer " + s_.apiKey};
        r.body = body.dump();
        r.timeoutSeconds = timeoutSeconds;
        return r;
    }

    Result<ChatReply> parseReply(const std::string& body) const override try {
        using R = Result<ChatReply>;
        const json j = json::parse(body);
        const json& choice = j.at("choices").at(0);
        const std::string finish = choice.value("finish_reason", "");
        if (finish == "length") return R::failure("llm.truncated", "大模型的回复被截断了（max_tokens 太小）");
        if (finish == "content_filter") return R::failure("llm.refusal", "大模型服务过滤掉了这次回复");
        ChatReply reply;
        const json& content = choice.at("message").at("content");
        reply.text = content.is_string() ? content.get<std::string>() : "";
        if (j.contains("usage")) {
            reply.inputTokens = j["usage"].value("prompt_tokens", 0);
            reply.outputTokens = j["usage"].value("completion_tokens", 0);
        }
        reply.model = j.value("model", "");
        return reply;
    } catch (const std::exception& e) {
        return Result<ChatReply>::failure("llm.bad_reply", std::string("看不懂大模型服务的回复：") + e.what());
    }

private:
    ProtocolSettings s_;
};

}  // namespace

std::unique_ptr<ChatProtocol> makeAnthropicProtocol(ProtocolSettings settings) {
    return std::make_unique<AnthropicProtocol>(std::move(settings));
}
std::unique_ptr<ChatProtocol> makeOpenAiCompatibleProtocol(ProtocolSettings settings) {
    return std::make_unique<OpenAiCompatibleProtocol>(std::move(settings));
}

}  // namespace vision::llm
