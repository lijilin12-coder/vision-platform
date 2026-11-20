#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vision/assistant/domain/ports.h"
#include "vision/llm/chat_protocol.h"
#include "vision/llm/http_client.h"

namespace vision::llm {

// 大模型顾问的配置。文件格式见 samples/assistant/llm.deepseek.json。
// 密钥不写在配置文件里：文件里只写“从哪个环境变量读”。
struct LlmConfig {
    std::string protocol = "openai";  // anthropic / openai
    std::string baseUrl;
    std::string model;
    std::string apiKeyEnv = "VISION_LLM_API_KEY";
    int maxTokens = 16000;
    int timeoutSeconds = 120;
    int maxRetries = 2;       // 服务繁忙、限流、超时时最多再试几次
    std::string effort;       // 只有 Anthropic 协议使用
    int maxImages = 6;        // 允许外发图像时，一轮最多附几幅
    int thumbnailSize = 256;  // 缩略图的长边（像素）
};
Result<LlmConfig> parseLlmConfig(const std::string& jsonText);
Result<std::unique_ptr<ChatProtocol>> makeProtocol(const LlmConfig& config, const std::string& apiKey);

// 把一幅图像读出来、按需要缩小、编码。maxSide 为 0 表示原图。
using ImageLoader = std::function<Result<ChatImage>(const std::string& imageRef, int maxSide)>;
ImageLoader fileImageLoader();  // 从文件读（OpenCV）

// 一次调用的记录
struct LlmCall {
    std::string requestBody;  // 发出去的完整内容（不含请求头，所以不含密钥）
    std::string replyText;
    int httpStatus = 0;
    int attempts = 0;
    double seconds = 0;
    int inputTokens = 0, outputTokens = 0;
    std::string error;
};

// 大模型顾问：方案顾问（RecipeAdvisor）的又一个实现。
//
// 每一轮：把助手给的内容（目标、可调项、评估、历史、各用例的判定和统计信息）写成一段文字，
// 发给大模型，请它回答一个 JSON 对象；把 JSON 解析成提议交回去。
// 它不驱动循环，不能直接修改方案；它的提议和其他顾问的一样，要先通过助手的规则检查。
//
//   - 大模型的回复看不懂：交回一份空的提议。助手会拒绝它并记录原因，循环继续。
//   - 服务不可用（断网、超时、重试之后仍然失败）：返回失败。助手以“出错”结束会话，保留已有的记录。
class LlmAdvisor final : public assistant::RecipeAdvisor {
public:
    LlmAdvisor(LlmConfig config, std::unique_ptr<ChatProtocol> protocol, HttpClient& http, ImageLoader images = {});

    std::string name() const override;
    Result<assistant::Proposal> propose(const assistant::AdvisorView& view) override;

    const std::vector<LlmCall>& calls() const { return calls_; }

    // 下面三个函数单独公开，方便测试
    static std::string systemPrompt();
    static std::string describe(const assistant::AdvisorView& view);     // 这一轮发给大模型的文字
    static assistant::Proposal parseProposal(const std::string& reply);  // 看不懂时返回空的提议

private:
    LlmConfig config_;
    std::unique_ptr<ChatProtocol> protocol_;
    HttpClient& http_;
    ImageLoader images_;
    std::vector<LlmCall> calls_;
};

}  // namespace vision::llm
