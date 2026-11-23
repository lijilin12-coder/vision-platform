#pragma once

// 带旋转角度的模板匹配。只在 operators/src 内部使用。
//
// 算法和 OpenCV 教程第 9 课大作业的参考实现是同一个
// （docs/tutorial/opencv/examples/capstone/reference/rotated_match.cpp），思路见那里的注释。
// 和教程版本的两处不同：
//   1. 角度范围是“0 度左右各 angleRange 度”，而不是从某个角度开始的一段——产线上零件一般只是摆歪了一点。
//   2. 不管得分高低都返回找到的最好结果，由调用者决定“多少分算找到”。

#include <optional>

#include <opencv2/core.hpp>

namespace vision::operators {

struct RotatedMatch {
    double x = 0;      // 模板中心在场景图像里的位置（像素，可以是小数）
    double y = 0;
    double angle = 0;  // 目标相对于模板转过的角度。度；屏幕上顺时针为正；(-180, 180]
    double score = 0;  // 相似程度，-1～1
};

// 在 scene（8 位灰度）里找 templ（8 位灰度）。angleRange：在 0 度左右各搜索多少度，0 表示不旋转。
// 只有根本没法找（模板比场景大）时返回空。
std::optional<RotatedMatch> findRotated(const cv::Mat& scene, const cv::Mat& templ, double angleRange);

}  // namespace vision::operators
