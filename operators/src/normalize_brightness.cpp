// 亮度归一化：把整幅图像乘以一个倍数，让它的中位灰度等于目标值。灰度图像 → 灰度图像。
//
// 用途：抵消光源变亮、变暗带来的整体亮度变化。后面的固定阈值就不用跟着光照变了。
// 用中位数而不用平均值：少量很亮的缺陷不会把它带偏。
// 前提：画面的大部分内容是稳定的（背景、零件本身）。如果缺陷占了画面的一半以上，这个办法不适用。
#include <algorithm>
#include <opencv2/imgproc.hpp>

#include "op_util.h"

namespace vision::operators {

class NormalizeBrightnessOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.normalize_brightness";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"image", PortType::Image, false}, {"gain", PortType::Number, false}};
        m.signature.params = {
            {"target", ParamType::Number, 128.0, 1.0, 254.0, {}, "归一化之后的中位灰度"},
        };
        m.displayName = "亮度归一化";
        m.category = "预处理";
        m.description = "按中位灰度把图像整体调亮或调暗到固定的水平，抵消光照的整体变化。设置了 ROI 时用 ROI 内的中位灰度";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = grayInput(ctx, "image", "operator.normalize_brightness", "亮度归一化");
        if (!in) return in.error();
        const cv::Mat src = imaging::view(in.value());
        auto area = workArea(ctx, src, "operator.normalize_brightness");
        if (!area) return area.error();

        // 中位数：用直方图找，比排序快
        int histogram[256] = {0};
        const cv::Mat part = src(area.value());
        for (int y = 0; y < part.rows; ++y) {
            const uchar* row = part.ptr<uchar>(y);
            for (int x = 0; x < part.cols; ++x) ++histogram[row[x]];
        }
        const long half = static_cast<long>(part.total()) / 2;
        long seen = 0;
        int median = 0;
        for (; median < 255; ++median) {
            seen += histogram[median];
            if (seen > half) break;
        }
        if (median == 0) return Status::failure("operator.normalize_brightness", "图像太暗（中位灰度为 0），无法归一化");

        const double gain = ctx.number("target") / median;
        cv::Mat out;
        src.convertTo(out, CV_8U, gain);  // 超过 255 的截断
        auto img = imaging::fromMat(out);
        if (!img) return img.error();
        ctx.setOutput("image", img.value());
        ctx.setOutput("gain", gain);
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::NormalizeBrightnessOp;
VISION_REGISTER_OPERATOR(NormalizeBrightnessOp)
