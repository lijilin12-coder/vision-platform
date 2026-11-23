// 形态学：修整区域的形状。区域 → 区域。
//   腐蚀：区域缩一圈（细小的部分消失）      膨胀：区域胀一圈（小洞被填上）
//   开运算 = 先腐蚀再膨胀：去掉小的噪点，大的部分基本不变
//   闭运算 = 先膨胀再腐蚀：填上小洞、连起靠得很近的几块，外形基本不变
#include <opencv2/imgproc.hpp>

#include "op_util.h"

namespace vision::operators {

class MorphologyOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.morphology";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"region", PortType::Region, false}};
        m.signature.outputs = {{"region", PortType::Region, false}};
        m.signature.params = {
            {"operation", ParamType::Choice, std::string("open"), std::nullopt, std::nullopt, {"open", "close", "erode", "dilate"}, "运算"},
            {"size", ParamType::Number, 3.0, 1.0, 101.0, {}, "结构元素的直径（像素，取奇数）"},
        };
        m.displayName = "形态学";
        m.category = "分割";
        m.description = "开运算去掉小噪点；闭运算填小洞、连接邻近的块；腐蚀、膨胀让区域缩小、扩大一圈";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Region>("region");
        if (!in) return in.error();
        const int size = oddSize(ctx.number("size"));
        const std::string& operation = ctx.text("operation");
        const int type = operation == "close"    ? cv::MORPH_CLOSE
                         : operation == "erode"  ? cv::MORPH_ERODE
                         : operation == "dilate" ? cv::MORPH_DILATE
                                                 : cv::MORPH_OPEN;
        cv::Mat out;
        cv::morphologyEx(imaging::view(in.value().mask()), out, type,
                         cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(size, size)));
        auto region = regionFromMask(out);
        if (!region) return region.error();
        ctx.setOutput("region", region.value());
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::MorphologyOp;
VISION_REGISTER_OPERATOR(MorphologyOp)
