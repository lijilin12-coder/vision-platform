// Blob 分析：区域 → 连通块列表，按面积过滤。
#include <opencv2/imgproc.hpp>

#include "vision/catalog/api.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::operators {

class BlobOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.blob";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"region", PortType::Region, false}};
        m.signature.outputs = {{"blobs", PortType::Blobs, false}, {"count", PortType::Number, false}};
        m.signature.params = {
            {"minArea", ParamType::Number, 1.0, 0.0, std::nullopt, {}, "最小面积（像素）"},
            {"maxArea", ParamType::Number, 1e9, 0.0, std::nullopt, {}, "最大面积（像素）"},
        };
        m.displayName = "Blob 分析";
        m.category = "分析";
        m.description = "把区域拆成连通块（8 邻域），计算面积、质心、外接框";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Region>("region");
        if (!in) return in.error();
        const double minArea = ctx.number("minArea");
        const double maxArea = ctx.number("maxArea");

        cv::Mat labels, stats, centroids;
        const int n = cv::connectedComponentsWithStats(imaging::view(in.value().mask()), labels, stats, centroids, 8);

        BlobList blobs;
        for (int i = 1; i < n; ++i) {  // 0 是背景
            const double area = stats.at<int>(i, cv::CC_STAT_AREA);
            if (area < minArea || area > maxArea) continue;
            Blob b;
            b.area = area;
            b.centerX = centroids.at<double>(i, 0);
            b.centerY = centroids.at<double>(i, 1);
            b.left = stats.at<int>(i, cv::CC_STAT_LEFT);
            b.top = stats.at<int>(i, cv::CC_STAT_TOP);
            b.width = stats.at<int>(i, cv::CC_STAT_WIDTH);
            b.height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
            blobs.push_back(b);
        }
        ctx.setOutput("count", static_cast<double>(blobs.size()));
        ctx.setOutput("blobs", std::move(blobs));
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::BlobOp;
VISION_REGISTER_OPERATOR(BlobOp)
