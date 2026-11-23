// 边缘强度：每个像素处灰度变化有多剧烈（Sobel 梯度的大小）。灰度图像 → 灰度图像。
//
// 用途：划痕、裂纹这类缺陷本身可能不比周围亮多少，但边缘很明显。先求边缘强度，再做阈值。
#include <opencv2/imgproc.hpp>

#include "op_util.h"

namespace vision::operators {

class EdgeMagnitudeOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.edge_magnitude";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"image", PortType::Image, false}};
        m.signature.params = {
            {"scale", ParamType::Number, 1.0, 0.01, 100.0, {}, "结果乘以这个倍数（边缘太弱看不清时调大）"},
        };
        m.displayName = "边缘强度";
        m.category = "预处理";
        m.description = "灰度变化越剧烈的地方越亮；平坦的地方接近 0。输出仍然是灰度图像，可以接阈值";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = grayInput(ctx, "image", "operator.edge_magnitude", "边缘强度");
        if (!in) return in.error();
        const cv::Mat src = imaging::view(in.value());
        cv::Mat gx, gy, magnitude, out;
        cv::Sobel(src, gx, CV_32F, 1, 0, 3);
        cv::Sobel(src, gy, CV_32F, 0, 1, 3);
        cv::magnitude(gx, gy, magnitude);
        // 3×3 的 Sobel 核对一个灰度差为 d 的理想边缘给出 4d；除以 4，让输出大致等于“两侧的灰度差”
        magnitude.convertTo(out, CV_8U, ctx.number("scale") / 4.0);
        auto img = imaging::fromMat(out);
        if (!img) return img.error();
        ctx.setOutput("image", img.value());
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::EdgeMagnitudeOp;
VISION_REGISTER_OPERATOR(EdgeMagnitudeOp)
