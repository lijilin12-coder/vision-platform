// 卡尺测宽：沿一个方向找到两条边缘，输出它们之间的距离（像素）。灰度图像 → 数值。
//
// 做法（和教程第 6 课“边缘”里的一维边缘检测是同一回事）：
//   1. 把测量范围（ROI，没有设置就是整幅图）沿垂直于测量方向压成一条“灰度曲线”：
//      每个位置取那一列（或那一行）的平均灰度。平均之后噪声小很多。
//   2. 对曲线求导：灰度变化最剧烈的位置就是边缘。
//   3. 取导数的绝对值超过 edgeThreshold 的峰。最靠前的一个和最靠后的一个，就是物体的两条外边缘。
//   4. 边缘在导数曲线上是一个“鼓包”，取鼓包的重心作为边缘的位置，比一个像素更精确（亚像素）。
//
// 找不到两条边缘时不算出错：found 输出 false，width 输出 0，由后面的判定算子去判不合格。
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "op_util.h"

namespace vision::operators {

class CaliperWidthOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.caliper_width";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"width", PortType::Number, false},
                               {"found", PortType::Bool, false},
                               {"edge1", PortType::Number, false},
                               {"edge2", PortType::Number, false}};
        m.signature.params = {
            {"direction", ParamType::Choice, std::string("horizontal"), std::nullopt, std::nullopt, {"horizontal", "vertical"},
             "测量方向：horizontal 量左右两条边之间的距离，vertical 量上下"},
            {"edgeThreshold", ParamType::Number, 10.0, 0.1, 255.0, {}, "边缘两侧的平均灰度至少相差多少才算一条边缘"},
            {"smooth", ParamType::Number, 3.0, 1.0, 51.0, {}, "求导之前对灰度曲线做平滑的宽度（像素，取奇数）"},
        };
        m.displayName = "卡尺测宽";
        m.category = "测量";
        m.description = "在 ROI 内沿测量方向找物体的两条外边缘，输出它们的距离（像素，亚像素精度）和各自的位置";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = grayInput(ctx, "image", "operator.caliper_width", "卡尺测宽");
        if (!in) return in.error();
        const cv::Mat src = imaging::view(in.value());
        auto area = workArea(ctx, src, "operator.caliper_width");
        if (!area) return area.error();
        const bool horizontal = ctx.text("direction") == "horizontal";

        // 1. 压成一条灰度曲线
        cv::Mat reduced;
        cv::reduce(src(area.value()), reduced, horizontal ? 0 : 1, cv::REDUCE_AVG, CV_64F);
        std::vector<double> profile(reduced.begin<double>(), reduced.end<double>());
        const int n = static_cast<int>(profile.size());
        const auto notFound = [&ctx]() {
            ctx.setOutput("width", 0.0);
            ctx.setOutput("found", false);
            ctx.setOutput("edge1", 0.0);
            ctx.setOutput("edge2", 0.0);
            return Status::success();
        };
        if (n < 5) return notFound();

        const int smooth = oddSize(ctx.number("smooth"));
        if (smooth > 1) {
            cv::Mat line(1, n, CV_64F, profile.data()), smoothed;
            cv::blur(line, smoothed, cv::Size(smooth, 1), cv::Point(-1, -1), cv::BORDER_REPLICATE);
            smoothed.copyTo(line);
        }

        // 2. 求导（中心差分）。d[i] 约等于位置 i 两侧相邻像素的灰度差的一半，所以乘 2 再和阈值比。
        std::vector<double> d(static_cast<std::size_t>(n), 0.0);
        for (int i = 1; i + 1 < n; ++i) d[static_cast<std::size_t>(i)] = (profile[i + 1] - profile[i - 1]) / 2.0;

        // 3. 找边缘。一条边缘在导数曲线上是一个“鼓包”：连续几个同号的值，中间最大。
        //    从左到右扫描；每遇到一个局部最大的位置，就向两边扩展到鼓包的范围
        //    （同号，并且不小于峰值的 10%——再小的是噪声带出来的尾巴，不算），
        //    鼓包里的值加起来就是这条边缘两侧的灰度差。一个鼓包只算一条边缘。
        const double threshold = ctx.number("edgeThreshold");
        const auto at = [&d](int i) { return d[static_cast<std::size_t>(i)]; };
        std::vector<double> edges;
        int i = 1;
        while (i + 1 < n) {
            const double peak = std::abs(at(i));
            if (peak <= 0 || peak < std::abs(at(i - 1)) || peak <= std::abs(at(i + 1))) {  // 不是峰
                ++i;
                continue;
            }
            const double sign = at(i) > 0 ? 1.0 : -1.0;
            const auto inBump = [&](int k) { return k >= 0 && k < n && at(k) * sign >= 0.1 * peak; };
            int first = i, last = i;
            while (inBump(first - 1)) --first;
            while (inBump(last + 1)) ++last;
            double sum = 0, weighted = 0;
            for (int k = first; k <= last; ++k) {
                sum += std::abs(at(k));
                weighted += k * std::abs(at(k));
            }
            // 4. 亚像素位置：鼓包的重心。理想的边缘落在两个像素之间的任何位置时，重心都正好是它的真实位置
            if (sum >= threshold) edges.push_back(weighted / sum);
            i = last + 1;  // 跳过整个鼓包：它里面即使还有小的起伏，也不再算另一条边缘
        }
        if (edges.size() < 2) return notFound();

        const double offset = horizontal ? area.value().x : area.value().y;  // 换回整幅图像里的坐标
        ctx.setOutput("width", edges.back() - edges.front());
        ctx.setOutput("found", true);
        ctx.setOutput("edge1", edges.front() + offset);
        ctx.setOutput("edge2", edges.back() + offset);
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::CaliperWidthOp;
VISION_REGISTER_OPERATOR(CaliperWidthOp)
