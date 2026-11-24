// 范围判定：数值在 [min, max] 之内为合格。输出判定依据（Judgement）。
// 用于尺寸测量（“宽度 152.3，允许 150 ~ 155”）、面积上限（min 取 0）等。
#include <iomanip>
#include <sstream>

#include "vision/catalog/api.h"

namespace vision::operators {

class JudgeRangeOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.judge_range";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"value", PortType::Number, false}};
        m.signature.outputs = {{"judgement", PortType::Judgement, false}};
        m.signature.params = {
            {"min", ParamType::Number, 0.0, std::nullopt, std::nullopt, {}, "允许的最小值"},
            {"max", ParamType::Number, 100.0, std::nullopt, std::nullopt, {}, "允许的最大值"},
            {"label", ParamType::Text, std::string("数值"), std::nullopt, std::nullopt, {}, "名称（用于判定依据），例如“宽度”"},
            {"unit", ParamType::Text, std::string(""), std::nullopt, std::nullopt, {}, "单位（用于判定依据），例如“像素”"},
        };
        m.displayName = "范围判定";
        m.category = "判定";
        m.description = "数值在最小值和最大值之间（含两端）为合格";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<double>("value");
        if (!in) return in.error();
        const double lo = ctx.number("min"), hi = ctx.number("max");
        if (lo > hi) return Status::failure("operator.judge_range", "范围判定的最小值大于最大值");
        const auto text = [](double v) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(2) << v;
            std::string s = os.str();
            s.erase(s.find_last_not_of('0') + 1);  // 152.30 → 152.3，150.00 → 150
            if (s.back() == '.') s.pop_back();
            return s;
        };
        const std::string& unit = ctx.text("unit");
        Judgement j;
        j.pass = in.value() >= lo && in.value() <= hi;
        j.reason = ctx.text("label") + " " + text(in.value()) + (unit.empty() ? "" : " " + unit) + "，允许 " + text(lo) + " ~ " + text(hi);
        ctx.setOutput("judgement", j);
        // 结果项：一个测量值。名称就是参数里的 label，同名的测量值可以跨检测放在一起看趋势
        Measurement m;
        m.name = ctx.text("label");
        m.value = in.value();
        m.unit = unit;
        m.lower = lo;
        m.upper = hi;
        m.pass = j.pass;
        ctx.reportMeasurement(std::move(m));
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::JudgeRangeOp;
VISION_REGISTER_OPERATOR(JudgeRangeOp)
