// 大作业：在这里写你的实现。
//
// 现在这个文件只是一个能编译的空壳：它永远返回“没找到”，所以评分程序会报告全部失败。
// 按任务书（docs/tutorial/opencv/09-capstone-rotated-matching.md）分阶段完成它。
#include "rotated_match.h"

#include <opencv2/imgproc.hpp>

namespace capstone {

std::optional<RotatedMatch> findRotated(const cv::Mat& scene, const cv::Mat& templ, const MatchOptions& options) {
    (void)scene;
    (void)templ;
    (void)options;
    return std::nullopt;  // TODO：第一阶段从这里开始
}

}  // namespace capstone
