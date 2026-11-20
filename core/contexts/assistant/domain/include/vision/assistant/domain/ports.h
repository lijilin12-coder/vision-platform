#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vision/assistant/domain/session.h"
#include "vision/assistant/domain/workflow.h"

namespace vision::assistant {

// 领域服务接口：方案顾问（D7-1）。
//
// “下一步该怎么改”是领域里的一个角色，所以接口定义在领域层；
// 谁来担任这个角色是实现细节：按脚本出牌的顾问、网格搜索、大模型……都只是它的一种实现，
// 放在适配器里，可以随时替换，领域层不知道它们的存在。
//
// 顾问不驱动循环（D7-2）：我们的代码每一轮问它一次，它只返回一份提议（数据）。
// 提议要先通过聚合的规则检查才会生效，所以顾问出错或者乱出主意，都不会破坏方案。
class RecipeAdvisor {
public:
    virtual ~RecipeAdvisor() = default;
    virtual std::string name() const = 0;
    virtual Result<Proposal> propose(const AdvisorView& view) = 0;
};

class WorkflowRepository {
public:
    virtual ~WorkflowRepository() = default;
    virtual std::optional<Workflow> find(const std::string& workflowId) const = 0;
    virtual std::vector<std::string> list() const = 0;
};

class AssistSessionRepository {
public:
    virtual ~AssistSessionRepository() = default;
    virtual Status save(const AssistSession& session) = 0;
    virtual std::optional<AssistSession> find(const SessionId& id) const = 0;
    virtual std::vector<SessionId> list() const = 0;
};

}  // namespace vision::assistant
