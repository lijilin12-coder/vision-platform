#pragma once

// 共享内核 Image 与 OpenCV cv::Mat 之间的转换。只有算子和适配器可以使用。

#include <opencv2/core.hpp>
#include <string>

#include "vision/common/result.h"
#include "vision/kernel/image.h"

namespace vision::imaging {

// 零拷贝：返回一个指向 Image 像素的只读 cv::Mat（不要修改它）。
cv::Mat view(const Image& image);

// 拷贝：从 cv::Mat 生成 Image。只支持 8 位单通道或三通道。
Result<Image> fromMat(const cv::Mat& mat);

// 图像文件读写
Result<Image> loadImage(const std::string& path);
Status saveImage(const std::string& path, const Image& image);

}  // namespace vision::imaging
