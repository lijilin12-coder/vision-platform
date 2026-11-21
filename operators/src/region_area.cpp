// 区域面积：区域 → 像素数，以及占画面（或 ROI）的百分比。
#include <opencv2/core.hpp>

#include "op_util.h"

namespace vision::operators {

class RegionAreaOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.region_area";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"region", PortType::Region, false}};
        m.signature.outputs = {{"area", PortType::Number, false}, {"percent", PortType::Number, false}};
        m.displayName = "区域面积";
        m.category = "分析";
        m.description = "区域的像素数，以及占整幅图像的百分比。设置了 ROI 时只数 ROI 内的，百分比也按 ROI 算";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Region>("region");
        if (!in) return in.error();
        const cv::Mat mask = imaging::view(in.value().mask());
        auto area = workArea(ctx, mask, "operator.region_area");
        if (!area) return area.error();
        const double pixels = cv::countNonZero(mask(area.value()));
        ctx.setOutput("area", pixels);
        ctx.setOutput("percent", 100.0 * pixels / static_cast<double>(area.value().area()));
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::RegionAreaOp;
VISION_REGISTER_OPERATOR(RegionAreaOp)
