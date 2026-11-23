#pragma once

// 几个算子都用得到的小工具。只在 operators/src 内部使用。

#include <opencv2/core.hpp>
#include <string>

#include "vision/catalog/api.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::operators {

// 算子实际处理的范围：设置了 ROI 就是 ROI 和图像的交集，否则是整幅图像。交集为空时返回失败。
inline Result<cv::Rect> workArea(const catalog::OpContext& ctx, const cv::Mat& image, const char* op) {
    cv::Rect area(0, 0, image.cols, image.rows);
    if (const auto& roi = ctx.roi()) {
        area &= cv::Rect(static_cast<int>(roi->x()), static_cast<int>(roi->y()), static_cast<int>(roi->width()),
                         static_cast<int>(roi->height()));
        if (area.empty()) return Result<cv::Rect>::failure(op, "ROI 不在图像范围内");
    }
    return area;
}

// 取灰度图像输入；不是灰度图像时给出让人看得懂的提示。
inline Result<Image> grayInput(const catalog::OpContext& ctx, const std::string& port, const char* op, const char* name) {
    auto in = ctx.input<Image>(port);
    if (!in) return in.error();
    if (in.value().format() != Image::Format::Gray8) {
        return Result<Image>::failure(op, std::string(name) + "需要灰度图像，请先连接“灰度”算子");
    }
    return in;
}

// 把参数里的数字变成大于等于 1 的奇数（很多 OpenCV 函数要求核的大小是奇数）
inline int oddSize(double value) {
    const int n = std::max(1, static_cast<int>(value + 0.5));
    return n % 2 == 1 ? n : n + 1;
}

// 单通道掩码 → 区域
inline Result<Region> regionFromMask(const cv::Mat& mask) {
    auto img = imaging::fromMat(mask);
    if (!img) return img.error();
    return Region::fromMask(img.value());
}

}  // namespace vision::operators
