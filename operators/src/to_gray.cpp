// 灰度转换
#include <opencv2/imgproc.hpp>

#include "vision/catalog/api.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::operators {

class ToGrayOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.to_gray";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"image", PortType::Image, false}};
        m.displayName = "灰度";
        m.category = "预处理";
        m.description = "彩色图像转为灰度图像；已经是灰度图像时原样输出";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Image>("image");
        if (!in) return in.error();
        if (in.value().format() == Image::Format::Gray8) {
            ctx.setOutput("image", in.value());
            return Status::success();
        }
        cv::Mat gray;
        cv::cvtColor(imaging::view(in.value()), gray, cv::COLOR_BGR2GRAY);
        auto out = imaging::fromMat(gray);
        if (!out) return out.error();
        ctx.setOutput("image", out.value());
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::ToGrayOp;
VISION_REGISTER_OPERATOR(ToGrayOp)
