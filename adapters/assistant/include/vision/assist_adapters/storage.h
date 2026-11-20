#pragma once

// 方案助手的 JSON 文件存储：工作流、助手会话。
// 适配器可以依赖领域层：它实现的是领域层定义的仓储接口。

#include <filesystem>
#include <string>

#include "vision/assistant/domain/ports.h"

namespace vision::assist_adapters {

// 工作流的 JSON 格式见 samples/workflows/bright-spot.json
Result<assistant::Workflow> parseWorkflow(const std::string& jsonText);
std::string workflowToJson(const assistant::Workflow& workflow);

// <root>/workflows/<workflowId>.json
class JsonWorkflowRepository final : public assistant::WorkflowRepository {
public:
    explicit JsonWorkflowRepository(std::filesystem::path root);
    // 导入一个工作流文件（先检查，再存进数据目录），返回工作流 ID
    Result<std::string> importFile(const std::filesystem::path& file);
    std::optional<assistant::Workflow> find(const std::string& workflowId) const override;
    std::vector<std::string> list() const override;

private:
    std::filesystem::path dir_;
};

// <root>/assist-sessions/<sessionId>.json
// 一个文件就是一次会话的完整记录：任务书、用的是哪个工作流（整份存进来，工作流以后改了也不影响旧记录）、
// 每一轮的提议和评估、盲测结果、报告。
class JsonAssistSessionRepository final : public assistant::AssistSessionRepository {
public:
    explicit JsonAssistSessionRepository(std::filesystem::path root);
    Status save(const assistant::AssistSession& session) override;
    std::optional<assistant::AssistSession> find(const assistant::SessionId& id) const override;
    std::vector<assistant::SessionId> list() const override;
    std::filesystem::path fileOf(const assistant::SessionId& id) const;

private:
    std::filesystem::path dir_;
};

}  // namespace vision::assist_adapters
