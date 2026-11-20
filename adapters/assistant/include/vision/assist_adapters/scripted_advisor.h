#pragma once

#include <string>
#include <vector>

#include "vision/assistant/domain/ports.h"

namespace vision::assist_adapters {

// 按脚本出牌的顾问：把事先写好的提议一份一份交出来，交完了就建议结束。
//
// 它不聪明，但它证明了一件事：循环、规则检查、评估、回退、报告都不依赖大模型。
// 脚本里可以故意写不合法的提议，用来验证助手的规则挡得住（POC-11）。
// 以后的大模型顾问（POC-12）只是 RecipeAdvisor 的另一个实现，其余代码一行都不用改。
class ScriptedAdvisor final : public assistant::RecipeAdvisor {
public:
    ScriptedAdvisor(std::string name, std::vector<assistant::Proposal> proposals);
    // 脚本的 JSON 格式见 samples/assistant/bright-spot.script.json
    static Result<ScriptedAdvisor> fromJson(const std::string& jsonText);

    std::string name() const override { return name_; }
    Result<assistant::Proposal> propose(const assistant::AdvisorView& view) override;

    // 每一轮顾问看到了什么（测试用：检查盲测集和图像有没有泄漏给顾问）
    const std::vector<assistant::AdvisorView>& seen() const { return seen_; }

private:
    std::string name_;
    std::vector<assistant::Proposal> proposals_;
    std::size_t next_ = 0;
    std::vector<assistant::AdvisorView> seen_;
};

}  // namespace vision::assist_adapters
