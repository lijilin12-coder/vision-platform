#pragma once

#include <string>
#include <vector>

#include "vision/common/result.h"
#include "vision/kernel/image.h"

namespace vision {

// ROI：人为指定的检测范围，是输入（术语表 §3）。POC 阶段只支持矩形。
class Roi {
public:
    static Result<Roi> rect(double x, double y, double width, double height);

    double x() const { return x_; }
    double y() const { return y_; }
    double width() const { return width_; }
    double height() const { return height_; }

    bool operator==(const Roi& o) const {
        return x_ == o.x_ && y_ == o.y_ && width_ == o.width_ && height_ == o.height_;
    }
    bool operator!=(const Roi& o) const { return !(*this == o); }

private:
    Roi(double x, double y, double w, double h) : x_(x), y_(y), width_(w), height_(h) {}
    double x_, y_, width_, height_;
};

// 区域：算法计算出的像素集合，是输出（术语表 §3）。用单通道掩码表示，非 0 即属于区域。
class Region {
public:
    Region() = default;
    static Result<Region> fromMask(Image mask);
    const Image& mask() const { return mask_; }

private:
    explicit Region(Image mask) : mask_(std::move(mask)) {}
    Image mask_;
};

// Blob：连通的一块区域及其特征。
struct Blob {
    double area = 0;
    double centerX = 0;
    double centerY = 0;
    int left = 0, top = 0, width = 0, height = 0;
};
using BlobList = std::vector<Blob>;

// 判定依据的原始形式：某个节点对“合不合格”的结论和原因（术语表 §6 判定依据）。
struct Judgement {
    bool pass = true;
    std::string reason;
};

}  // namespace vision
