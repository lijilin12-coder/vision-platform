// 自适应阈值：每个像素和它周围一小块的平均灰度比，而不是和一个固定的值比。灰度图像 → 区域。
//
// 用途：光照不均匀（一边亮一边暗）时，固定阈值顾此失彼；自适应阈值只看“比周围亮多少”。
// 注意：它找的是“局部比周围亮（或暗）”的地方。比 blockSize 还大的一整片亮区，只有边缘会被选中。
#include <opencv2/imgproc.hpp>

#include "op_util.h"

namespace vision::operators {

class AdaptiveThresholdOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.adaptive_threshold";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"region", PortType::Region, false}};
        m.signature.params = {
            {"blockSize", ParamType::Number, 51.0, 3.0, 501.0, {}, "比较范围的边长（像素，取奇数）。应该明显大于要找的目标"},
            {"offset", ParamType::Number, 20.0, 0.0, 255.0, {}, "比周围的平均灰度亮（或暗）多少才算"},
            {"invert", ParamType::Bool, false, std::nullopt, std::nullopt, {}, "取比周围暗的像素"},
        };
        m.displayName = "自适应阈值";
        m.category = "分割";
        m.description = "比周围一小块的平均灰度亮 offset 以上的像素组成区域（invert 时取暗的）。适合光照不均匀的图像";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = grayInput(ctx, "image", "operator.adaptive_threshold", "自适应阈值");
        if (!in) return in.error();
        const cv::Mat src = imaging::view(in.value());
        auto area = workArea(ctx, src, "operator.adaptive_threshold");
        if (!area) return area.error();

        const int block = std::max(3, oddSize(ctx.number("blockSize")));
        const bool invert = ctx.flag("invert");
        // OpenCV 的规则是“像素 > 局部平均 - C”。要“比平均亮 offset”，C 取 -offset；取暗的时候 C 取 +offset。
        const double c = invert ? ctx.number("offset") : -ctx.number("offset");
        cv::Mat mask = cv::Mat::zeros(src.size(), CV_8UC1);
        cv::Mat dst = mask(area.value());
        cv::adaptiveThreshold(src(area.value()), dst, 255, cv::ADAPTIVE_THRESH_MEAN_C,
                              invert ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY, block, c);

        auto region = regionFromMask(mask);
        if (!region) return region.error();
        ctx.setOutput("region", region.value());
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::AdaptiveThresholdOp;
VISION_REGISTER_OPERATOR(AdaptiveThresholdOp)
