#include "vision/imaging/cv_bridge.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>

#include "vision/common/path.h"

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

// 读写图像文件时不使用 cv::imread / cv::imwrite：它们在 Windows 上不支持带中文的路径。
// 改为自己读写文件字节（路径按 UTF-8 处理），再用 imdecode / imencode 在内存中编解码。

Result<Image> loadImage(const std::string& path) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) return Result<Image>::failure("imaging.read", "无法打开图像文件：" + path);
    const std::vector<uchar> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return Result<Image>::failure("imaging.read", "图像文件为空：" + path);
    cv::Mat m = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
    if (m.empty()) return Result<Image>::failure("imaging.read", "无法解码图像：" + path);
    if (m.depth() != CV_8U) m.convertTo(m, CV_8U);
    if (m.channels() == 4) cv::cvtColor(m, m, cv::COLOR_BGRA2BGR);
    return fromMat(m);
}

Status saveImage(const std::string& path, const Image& image) {
    if (image.empty()) return Status::failure("imaging.empty", "图像为空");
    const std::filesystem::path file = pathFromUtf8(path);
    std::string ext = pathToUtf8(file.extension());
    if (ext.empty()) ext = ".png";
    std::vector<uchar> bytes;
    try {
        if (!cv::imencode(ext, view(image), bytes)) {
            return Status::failure("imaging.write", "无法编码图像：" + path);
        }
    } catch (const cv::Exception&) {
        return Status::failure("imaging.write", "不支持的图像格式：" + ext);
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) return Status::failure("imaging.write", "无法写入图像：" + path);
    return Status::success();
}

}  // namespace vision::imaging
