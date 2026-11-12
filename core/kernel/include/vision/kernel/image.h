#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "vision/common/result.h"

namespace vision {

// 图像（值对象）。不可变；复制时共享同一块像素内存，不拷贝像素。
// 共享内核不依赖 OpenCV：算子通过 adapters/imaging 的 cv_bridge 与 cv::Mat 互相转换。
class Image {
public:
    enum class Format { Gray8, Bgr8 };

    Image() = default;

    // 像素数量必须等于 width * height * channels。
    static Result<Image> create(int width, int height, Format format, std::vector<std::uint8_t> pixels);

    int width() const { return width_; }
    int height() const { return height_; }
    Format format() const { return format_; }
    int channels() const { return format_ == Format::Gray8 ? 1 : 3; }
    std::size_t rowBytes() const { return static_cast<std::size_t>(width_) * channels(); }
    bool empty() const { return !pixels_ || pixels_->empty(); }
    const std::uint8_t* data() const { return empty() ? nullptr : pixels_->data(); }

private:
    int width_ = 0;
    int height_ = 0;
    Format format_ = Format::Gray8;
    std::shared_ptr<const std::vector<std::uint8_t>> pixels_;
};

}  // namespace vision
