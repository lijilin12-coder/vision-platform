// 计数判定：Blob 数量超过上限即不合格。输出判定依据（Judgement）。
#include <cmath>

#include "vision/catalog/api.h"

namespace vision::operators {

class JudgeCountOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.judge_count";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"blobs", PortType::Blobs, false}};
        m.signature.outputs = {{"judgement", PortType::Judgement, false}};
        m.signature.params = {
            {"maxCount", ParamType::Number, 0.0, 0.0, std::nullopt, {}, "允许的最大数量"},
            {"label", ParamType::Text, std::string("缺陷"), std::nullopt, std::nullopt, {}, "名称（用于判定依据）"},
        };
        m.displayName = "计数判定";
        m.category = "判定";
        m.description = "数量不超过上限为合格";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<BlobList>("blobs");
        if (!in) return in.error();
        const auto count = static_cast<long>(in.value().size());
        const auto limit = static_cast<long>(std::floor(ctx.number("maxCount")));
        const std::string& label = ctx.text("label");
        Judgement j;
        j.pass = count <= limit;
        j.reason = label + " " + std::to_string(count) + " 处，上限 " + std::to_string(limit) + " 处";
        ctx.setOutput("judgement", j);
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::JudgeCountOp;
VISION_REGISTER_OPERATOR(JudgeCountOp)
