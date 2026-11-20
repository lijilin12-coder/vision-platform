// 读图像文件、缩小、编码成 JPEG、转成 base64。只有允许外发图像时才会用到。
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "vision/common/path.h"
#include "vision/llm/llm_advisor.h"

namespace vision::llm {

namespace {

std::string base64(const std::vector<unsigned char>& data) {
    static const char* kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const unsigned b0 = data[i], b1 = i + 1 < data.size() ? data[i + 1] : 0, b2 = i + 2 < data.size() ? data[i + 2] : 0;
        out += kTable[b0 >> 2];
        out += kTable[((b0 & 3) << 4) | (b1 >> 4)];
        out += i + 1 < data.size() ? kTable[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        out += i + 2 < data.size() ? kTable[b2 & 63] : '=';
    }
    return out;
}

}  // namespace

ImageLoader fileImageLoader() {
    return [](const std::string& imageRef, int maxSide) -> Result<ChatImage> {
        using R = Result<ChatImage>;
        cv::Mat image = cv::imread(imageRef, cv::IMREAD_COLOR);
        if (image.empty()) return R::failure("llm.image", "无法读取图像 " + imageRef);
        const int side = std::max(image.cols, image.rows);
        if (maxSide > 0 && side > maxSide) {
            const double scale = static_cast<double>(maxSide) / side;
            cv::resize(image, image, cv::Size(), scale, scale, cv::INTER_AREA);
        }
        std::vector<unsigned char> encoded;
        if (!cv::imencode(".jpg", image, encoded, {cv::IMWRITE_JPEG_QUALITY, 85})) return R::failure("llm.image", "图像编码失败");
        return ChatImage{"image/jpeg", base64(encoded), ""};
    };
}

}  // namespace vision::llm
