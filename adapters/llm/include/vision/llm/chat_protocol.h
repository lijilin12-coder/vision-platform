#pragma once

#include <memory>
#include <string>
#include <vector>

#include "vision/common/result.h"
#include "vision/llm/http_client.h"

namespace vision::llm {

// 随消息附带的一幅图像（已经编码好的 PNG 或 JPEG）
struct ChatImage {
    std::string mediaType;  // image/png、image/jpeg
    std::string base64;
    std::string caption;    // 这是哪个用例的图
};

struct ChatRequest {
    std::string system;  // 系统提示词：角色和规则
    std::string user;    // 这一轮的内容
    std::vector<ChatImage> images;
};

struct ChatReply {
    std::string text;
    int inputTokens = 0;
    int outputTokens = 0;
    std::string model;  // 实际回答的是哪个模型
};

struct ProtocolSettings {
    std::string baseUrl;
    std::string model;
    std::string apiKey;
    int maxTokens = 16000;
    std::string effort;  // 只有 Anthropic 协议使用；空表示用模型的默认值
};

// 一种大模型服务的对话协议：怎么把一次对话写成 HTTP 请求，怎么从回复里取出文字。
// 不同厂商只在这里不同；提示词、回复的解析、重试都与协议无关。
class ChatProtocol {
public:
    virtual ~ChatProtocol() = default;
    virtual std::string name() const = 0;
    virtual HttpRequest buildRequest(const ChatRequest& chat, int timeoutSeconds) const = 0;
    // 状态码 200 的回复。模型拒绝回答、回复被截断等情况返回失败。
    virtual Result<ChatReply> parseReply(const std::string& body) const = 0;
};

// Anthropic（Claude）的 Messages 接口
std::unique_ptr<ChatProtocol> makeAnthropicProtocol(ProtocolSettings settings);
// OpenAI 兼容的 chat/completions 接口：DeepSeek、通义千问、智谱、Kimi 等国内服务都用它
std::unique_ptr<ChatProtocol> makeOpenAiCompatibleProtocol(ProtocolSettings settings);

}  // namespace vision::llm
