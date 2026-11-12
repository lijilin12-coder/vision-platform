// 反色：255 - 像素值。这是最简单的算子，作为《如何新增一个算子》的示例（docs/dev/how-to-add-operator.md）。
#include <opencv2/core.hpp>

#include "vision/catalog/api.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::operators {

class InvertOp final : public catalog::Operator {
public:
    // 1. 元数据：算子 ID、主版本号、端口、参数。编辑器据此自动生成节点和参数面板。
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.invert";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"image", PortType::Image, false}};
        m.displayName = "反色";
        m.category = "预处理";
        m.description = "每个像素变为 255 减去原值";
        return m;
    }

    // 2. 执行：读输入 → 计算 → 写输出。出错时返回错误，不要抛异常。
    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Image>("image");
        if (!in) return in.error();
        cv::Mat out;
        cv::bitwise_not(imaging::view(in.value()), out);
        auto img = imaging::fromMat(out);
        if (!img) return img.error();
        ctx.setOutput("image", img.value());
        return Status::success();
    }
};

}  // namespace vision::operators

// 3. 注册：一行。
using vision::operators::InvertOp;
VISION_REGISTER_OPERATOR(InvertOp)
