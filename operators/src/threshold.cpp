// 阈值分割：灰度图像 → 区域。设置了 ROI 时只在 ROI 内分割。
#include <algorithm>
#include <opencv2/imgproc.hpp>

#include "vision/catalog/api.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::operators {

class ThresholdOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.threshold";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"region", PortType::Region, false}};
        m.signature.params = {
            {"method", ParamType::Choice, std::string("fixed"), std::nullopt, std::nullopt, {"fixed", "otsu"}, "方法"},
            {"value", ParamType::Number, 128.0, 0.0, 255.0, {}, "阈值（fixed 时有效）"},
            {"invert", ParamType::Bool, false, std::nullopt, std::nullopt, {}, "取暗区域"},
        };
        m.displayName = "阈值";
        m.category = "分割";
        m.description = "灰度大于阈值的像素组成区域（invert 时取小于阈值的像素）";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Image>("image");
        if (!in) return in.error();
        if (in.value().format() != Image::Format::Gray8) {
            return Status::failure("operator.threshold", "阈值分割需要灰度图像，请先连接“灰度”算子");
        }
        const cv::Mat src = imaging::view(in.value());
        const int type = ctx.flag("invert") ? cv::THRESH_BINARY_INV : cv::THRESH_BINARY;
        const bool otsu = ctx.text("method") == "otsu";

        cv::Mat mask = cv::Mat::zeros(src.size(), CV_8UC1);
        cv::Rect area(0, 0, src.cols, src.rows);
        if (const auto& roi = ctx.roi()) {
            area &= cv::Rect(static_cast<int>(roi->x()), static_cast<int>(roi->y()), static_cast<int>(roi->width()),
                             static_cast<int>(roi->height()));
            if (area.empty()) return Status::failure("operator.threshold", "ROI 不在图像范围内");
        }
        cv::Mat dst = mask(area);
        cv::threshold(src(area), dst, ctx.number("value"), 255, type | (otsu ? cv::THRESH_OTSU : 0));

        auto img = imaging::fromMat(mask);
        if (!img) return img.error();
        auto region = Region::fromMask(img.value());
        if (!region) return region.error();
        ctx.setOutput("region", region.value());
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::ThresholdOp;
VISION_REGISTER_OPERATOR(ThresholdOp)
