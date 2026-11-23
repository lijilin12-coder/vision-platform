// 按位姿对齐：把图像平移、旋转，让零件回到框模板时的位置（docs/ddd/09 §6.2）。
//
// 对齐之后，后面的算子、ROI、卡尺都可以当作“零件每次都在同一个位置”来用。
// 代价：整幅图像重新采样一次，边缘会轻微变模糊。
#include <cmath>
#include <opencv2/imgproc.hpp>

#include "op_util.h"

namespace vision::operators {

class AlignToPoseOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.align_to_pose";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}, {"pose", PortType::Pose, false}};
        m.signature.outputs = {{"image", PortType::Image, false}};
        m.signature.params = {
            {"border", ParamType::Choice, std::string("constant"), std::nullopt, std::nullopt, {"constant", "replicate"},
             "对齐之后空出来的地方怎么填：constant 填固定的灰度（fill）；replicate 把图像边上的像素延伸出去"},
            {"fill", ParamType::Number, 0.0, 0.0, 255.0, {}, "border 为 constant 时填什么灰度"},
        };
        m.displayName = "按位姿对齐";
        m.category = "定位";
        m.description = "按模板匹配给出的位姿把图像转正、移回基准位置。输出的图像和输入一样大";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Image>("image");
        if (!in) return in.error();
        auto pose = ctx.input<Pose>("pose");
        if (!pose) return pose.error();
        const cv::Mat src = imaging::view(in.value());
        const Pose& p = pose.value();

        // 对齐后的图像里的一点 q（就是样图坐标），对应输入图像里的哪一点？
        //   零件上的点在样图里是 q，现在在  R·(q − 基准点) + (x, y)
        // 其中 R 是转 angle 度的旋转矩阵。图像坐标 y 向下，所以这个矩阵在屏幕上看是顺时针转：
        //   R = [ cos  −sin ]
        //       [ sin   cos ]
        const double radians = p.angle * CV_PI / 180.0;
        const double c = std::cos(radians), s = std::sin(radians);
        cv::Mat m = (cv::Mat_<double>(2, 3) << c, -s, p.x - (c * p.referenceX - s * p.referenceY),
                     s, c, p.y - (s * p.referenceX + c * p.referenceY));

        const double fill = ctx.number("fill");
        cv::Mat out;
        // WARP_INVERSE_MAP：m 给的是“输出 → 输入”的对应关系，正好是上面推出来的
        // replicate：空出来的地方接着图像边缘的灰度往外延伸，不会凭空多出一条“画面 / 填充色”的边缘。
        // 后面要找边缘（卡尺、边缘强度）时用它；constant 填的颜色和背景不一样时，那条边会被当成真的边缘。
        const int border = ctx.text("border") == "replicate" ? cv::BORDER_REPLICATE : cv::BORDER_CONSTANT;
        cv::warpAffine(src, out, m, src.size(), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, border, cv::Scalar(fill, fill, fill));
        auto img = imaging::fromMat(out);
        if (!img) return img.error();
        ctx.setOutput("image", img.value());
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::AlignToPoseOp;
VISION_REGISTER_OPERATOR(AlignToPoseOp)
