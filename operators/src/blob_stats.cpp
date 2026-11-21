// Blob 统计：连通块列表 → 个数、总面积、最大的一块的面积。
// 配合“范围判定”可以表达“缺陷总面积不超过多少”“最大的缺陷不超过多少”。
#include <algorithm>

#include "vision/catalog/api.h"

namespace vision::operators {

class BlobStatsOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.blob_stats";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"blobs", PortType::Blobs, false}};
        m.signature.outputs = {{"count", PortType::Number, false},
                               {"totalArea", PortType::Number, false},
                               {"maxArea", PortType::Number, false}};
        m.displayName = "Blob 统计";
        m.category = "分析";
        m.description = "连通块的个数、面积之和、最大一块的面积（没有连通块时都是 0）";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<BlobList>("blobs");
        if (!in) return in.error();
        double total = 0, largest = 0;
        for (const auto& b : in.value()) {
            total += b.area;
            largest = std::max(largest, b.area);
        }
        ctx.setOutput("count", static_cast<double>(in.value().size()));
        ctx.setOutput("totalArea", total);
        ctx.setOutput("maxArea", largest);
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::BlobStatsOp;
VISION_REGISTER_OPERATOR(BlobStatsOp)
