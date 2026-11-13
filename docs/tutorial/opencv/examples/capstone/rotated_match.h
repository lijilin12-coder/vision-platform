#pragma once

// 大作业：带旋转角度的模板匹配。接口定义。
//
// 你要做的是在 student/rotated_match.cpp 里实现下面的 findRotated 函数。
// 评分程序（evaluate.cpp）只通过这个头文件使用你的实现，所以不要改这个文件。

#include <optional>

#include <opencv2/core.hpp>

namespace capstone {

// 一个匹配结果
struct RotatedMatch {
    double x = 0;      // 模板中心在场景图像里的位置（像素，可以是小数）
    double y = 0;
    double angle = 0;  // 目标相对于模板转过的角度。单位：度；范围 [0, 360)；在屏幕上看，正的角度是顺时针
    double score = 0;  // 相似程度，-1～1，越大越像
};

struct MatchOptions {
    double minScore = 0.7;     // 得分低于它，就认为没有找到
    double angleStart = 0;     // 搜索的角度范围：从 angleStart 开始，
    double angleExtent = 360;  // 顺时针方向一共 angleExtent 度。默认是整圈
};

// 在 scene（8 位灰度图像）里找 templ（8 位灰度图像）。
//   找到：返回位置、角度和得分。
//   没找到（没有任何位置的得分达到 options.minScore）：返回 std::nullopt。
//
// “模板中心”指模板图像的 (templ.cols / 2.0, templ.rows / 2.0) 这一点。
// 角度为 0 表示目标和模板的朝向相同。
std::optional<RotatedMatch> findRotated(const cv::Mat& scene, const cv::Mat& templ, const MatchOptions& options = {});

}  // namespace capstone
