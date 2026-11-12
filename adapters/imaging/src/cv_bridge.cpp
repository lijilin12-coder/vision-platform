#include "vision/imaging/cv_bridge.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace vision::imaging {

cv::Mat view(const Image& image) {
    if (image.empty()) return {};
    const int type = image.channels() == 1 ? CV_8UC1 : CV_8UC3;
    return cv::Mat(image.height(), image.width(), type, const_cast<std::uint8_t*>(image.data()), image.rowBytes());
}

Result<Image> fromMat(const cv::Mat& mat) {
    if (mat.empty()) return Result<Image>::failure("imaging.empty", "图像为空");
    if (mat.depth() != CV_8U || (mat.channels() != 1 && mat.channels() != 3)) {
        return Result<Image>::failure("imaging.format", "只支持 8 位单通道或三通道图像");
    }
    cv::Mat m = mat.isContinuous() ? mat : mat.clone();
    std::vector<std::uint8_t> pixels(m.data, m.data + m.total() * m.elemSize());
    return Image::create(m.cols, m.rows, m.channels() == 1 ? Image::Format::Gray8 : Image::Format::Bgr8,
                         std::move(pixels));
}

Result<Image> loadImage(const std::string& path) {
    cv::Mat m = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (m.empty()) return Result<Image>::failure("imaging.read", "无法读取图像：" + path);
    if (m.depth() != CV_8U) m.convertTo(m, CV_8U);
    if (m.channels() == 4) cv::cvtColor(m, m, cv::COLOR_BGRA2BGR);
    return fromMat(m);
}

Status saveImage(const std::string& path, const Image& image) {
    if (image.empty()) return Status::failure("imaging.empty", "图像为空");
    if (!cv::imwrite(path, view(image))) return Status::failure("imaging.write", "无法写入图像：" + path);
    return Status::success();
}

}  // namespace vision::imaging
