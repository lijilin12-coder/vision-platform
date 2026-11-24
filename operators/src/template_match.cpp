// 模板匹配：在图像里找到匹配模板，输出位姿（docs/ddd/09-match-template-and-locating.md §6.1）。
//
// 没找到不算出错：found 输出 false，位姿取基准位置，score 是找到的最好的分数。
// “零件没放好”是正常的生产情况，由后面的判定算子去判不合格。
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

#include "op_util.h"
#include "rotated_matcher.h"

namespace vision::operators {

class TemplateMatchOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.template_match";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"pose", PortType::Pose, false}, {"score", PortType::Number, false}, {"found", PortType::Bool, false}};
        m.signature.params = {
            {"template", ParamType::Template, std::string(""), std::nullopt, std::nullopt, {}, "匹配模板"},
            {"angleRange", ParamType::Number, 15.0, 0.0, 180.0, {}, "在 0 度左右各搜索多少度（0 表示零件不会转）。范围越大越慢"},
            {"minScore", ParamType::Number, 0.7, 0.0, 1.0, {}, "相似程度低于它算没找到（0～1）"},
        };
        m.displayName = "模板匹配";
        m.category = "定位";
        m.description = "在图像里找到和匹配模板最像的位置和角度，输出位姿。设置了 ROI 时只在 ROI 里找";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        const char* op = "operator.template_match";
        auto in = grayInput(ctx, "image", op, "模板匹配");
        if (!in) return in.error();
        auto t = ctx.matchTemplate("template");
        if (!t) return t.error();
        const cv::Mat scene = imaging::view(in.value());
        if (t.value().image.format() != Image::Format::Gray8) {
            return Status::failure(op, "匹配模板不是灰度图像：请从灰度的样图上框选模板");
        }
        const cv::Mat templ = imaging::view(t.value().image);

        // 模板是按像素定义的：换了分辨率，它就不适用了。明确报错，而不是给一个很低的分数让人去猜。
        if (scene.cols != t.value().sourceWidth || scene.rows != t.value().sourceHeight) {
            return Status::failure(op, "图像是 " + std::to_string(scene.cols) + "×" + std::to_string(scene.rows) +
                                           "，而这个模板是在 " + std::to_string(t.value().sourceWidth) + "×" +
                                           std::to_string(t.value().sourceHeight) + " 的图像上框的。模板只适用于同样尺寸的图像");
        }
        auto area = workArea(ctx, scene, op);
        if (!area) return area.error();

        // 基准点：模板的中心。它在样图上的位置：
        Pose pose;
        pose.referenceX = t.value().referenceX + templ.cols / 2.0;
        pose.referenceY = t.value().referenceY + templ.rows / 2.0;
        pose.x = pose.referenceX;
        pose.y = pose.referenceY;

        double score = 0;
        bool found = false;
        if (const auto match = findRotated(scene(area.value()), templ, ctx.number("angleRange"))) {
            // 夹到 0～1：完全一样的图像算出来可能是 1.0000001，接到“范围判定 0.7～1”上会被判成超出范围
            score = std::clamp(match->score, 0.0, 1.0);
            found = match->score >= ctx.number("minScore");
            if (found) {
                pose.x = match->x + area.value().x;  // 换回整幅图像里的坐标
                pose.y = match->y + area.value().y;
                pose.angle = match->angle;
            }
        }
        ctx.setOutput("pose", pose);
        ctx.setOutput("score", score);
        ctx.setOutput("found", found);
        // 结果项：定位的结果。没找到也上报（found = false），下游才知道“这一次没定位上”
        ctx.reportPose(PoseResult{"", pose, found, score});
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::TemplateMatchOp;
VISION_REGISTER_OPERATOR(TemplateMatchOp)
