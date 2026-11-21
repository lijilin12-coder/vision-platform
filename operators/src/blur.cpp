// 平滑：去掉噪点。图像 → 图像。
#include <opencv2/imgproc.hpp>

#include "op_util.h"

namespace vision::operators {

class BlurOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.blur";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"image", PortType::Image, false}};
        m.signature.params = {
            {"method", ParamType::Choice, std::string("gaussian"), std::nullopt, std::nullopt, {"gaussian", "median", "mean"}, "方法"},
            {"size", ParamType::Number, 5.0, 1.0, 31.0, {}, "核的大小（像素，取奇数；1 表示不平滑）"},
        };
        m.displayName = "平滑";
        m.category = "预处理";
        m.description = "高斯：一般的噪声；中值：孤立的亮点或暗点，并且不模糊边缘；均值：最快";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Image>("image");
        if (!in) return in.error();
        const cv::Mat src = imaging::view(in.value());
        const int size = oddSize(ctx.number("size"));
        const std::string& method = ctx.text("method");
        cv::Mat out;
        if (size == 1) {
            out = src.clone();
        } else if (method == "median") {
            cv::medianBlur(src, out, size);
        } else if (method == "mean") {
            cv::blur(src, out, cv::Size(size, size));
        } else {
            cv::GaussianBlur(src, out, cv::Size(size, size), 0);
        }
        auto img = imaging::fromMat(out);
        if (!img) return img.error();
        ctx.setOutput("image", img.value());
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::BlurOp;
VISION_REGISTER_OPERATOR(BlurOp)
