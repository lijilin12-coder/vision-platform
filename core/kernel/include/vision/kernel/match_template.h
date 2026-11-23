#pragma once

#include "vision/kernel/image.h"

namespace vision {

// 算子拿到手的匹配模板：解码好的图像，加上它在样图上的位置（docs/ddd/09-match-template-and-locating.md）。
// 算子不接触文件、内容哈希这些存储上的细节。
struct MatchTemplateData {
    Image image;           // 模板图像
    double referenceX = 0; // 基准位置：模板左上角在样图上的位置（像素）
    double referenceY = 0;
    int sourceWidth = 0;   // 样图的尺寸。模板是按像素定义的，只适用于同样尺寸的图像
    int sourceHeight = 0;
};

}  // namespace vision
