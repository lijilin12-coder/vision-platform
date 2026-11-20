#pragma once

// 把方案助手的端口接到方案构建平台的其他上下文上。
// 只有这个文件同时认识助手和其他上下文；助手自己不认识它们。

#include <map>
#include <mutex>

#include "vision/assistant/api.h"
#include "vision/authoring/api.h"
#include "vision/execution/api.h"
#include "vision/release/api.h"

namespace vision::assist_adapters {

// 候选方案的编辑 → 方案编排的编辑命令。这里没有、也不会有 publish（A1）。
class AuthoringCandidateEditor final : public assistant::CandidateEditor {
public:
    explicit AuthoringCandidateEditor(authoring::AuthoringService& authoring) : authoring_(authoring) {}
    Result<std::string> createRecipe(const std::string& name) override { return authoring_.createRecipe(name); }
    Result<std::string> addNode(const std::string& recipeId, const std::string& operatorId) override {
        return authoring_.addNode(recipeId, operatorId);
    }
    Status connect(const std::string& recipeId, const std::string& from, const std::string& to) override {
        return authoring_.connect(recipeId, from, to);
    }
    Status setParam(const std::string& recipeId, const std::string& nodeId, const std::string& param,
                    const ParamValue& value) override {
        return authoring_.setParam(recipeId, nodeId, param, value);
    }

private:
    authoring::AuthoringService& authoring_;
};

// 试运行 → 取候选方案的当前草稿，交给检测引擎调试运行。
class EngineTrialRunner final : public assistant::TrialRunner {
public:
    EngineTrialRunner(const authoring::AuthoringService& authoring, execution::ExecutionService& execution)
        : authoring_(authoring), execution_(execution) {}
    Result<assistant::TrialResult> run(const std::string& recipeId, const std::string& imageRef,
                                       const std::optional<assistant::Perturbation>& perturbation) override;

private:
    Result<Image> imageFor(const std::string& imageRef);

    const authoring::AuthoringService& authoring_;
    execution::ExecutionService& execution_;
    std::map<std::string, Image> images_;  // 同一幅图每一轮都要用，读一次就够了
};

// 基准图集和评估规则 → 上线管理。
class ReleaseGoldenCases final : public assistant::GoldenCases {
public:
    explicit ReleaseGoldenCases(const release::GoldenSetService& golden) : golden_(golden) {}
    Result<std::vector<assistant::CaseInfo>> cases(const std::string& goldenSetId) override;
    Result<assistant::EvaluationData> evaluate(const std::string& goldenSetId,
                                               const std::vector<assistant::CaseOutcome>& outcomes) override;
    bool betterThan(const assistant::EvaluationData& a, const assistant::EvaluationData& b) override;

private:
    const release::GoldenSetService& golden_;
};

// 对图像做已知的扰动（稳健性检查）。同样的图像和参数，结果总是一样。
Result<Image> perturb(const Image& image, const assistant::Perturbation& perturbation, unsigned seed);

}  // namespace vision::assist_adapters
