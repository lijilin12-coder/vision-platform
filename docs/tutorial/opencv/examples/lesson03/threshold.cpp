// 第 3 课：阈值分割。固定阈值、大津法、自适应阈值，以及它们各自什么时候失效。
#include <cmath>

#include "cvtut.h"

// 零件的真实面积（像素）：360×260 的矩形，减去两个半径 35 的圆孔。划痕很小，忽略。
const double kPartArea = 360.0 * 260.0 - 2 * 3.14159265 * 35 * 35;

// 分割出来的面积和真实面积相差的比例
double areaError(const cv::Mat& mask) { return std::abs(cv::countNonZero(mask) - kPartArea) / kPartArea; }

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    const cv::Mat part = cvtut::makePartImage();

    // ---------- 1. 固定阈值 ----------
    // THRESH_BINARY：大于阈值的变成 255，其余变成 0 → 得到的是亮的背景
    cv::Mat bright;
    cv::threshold(part, bright, 120, 255, cv::THRESH_BINARY);
    // THRESH_BINARY_INV：反过来，不大于阈值的变成 255 → 得到的是暗的零件
    cv::Mat partMask;
    cv::threshold(part, partMask, 120, 255, cv::THRESH_BINARY_INV);
    cvtut::save(out / "03_fixed_120.png", partMask);

    c.check(cv::countNonZero(bright) + cv::countNonZero(partMask) == static_cast<int>(part.total()), "两种结果正好互补");
    std::cout << "固定阈值 120：零件面积 " << cv::countNonZero(partMask) << "，真实面积约 " << kPartArea << "，相差 "
              << areaError(partMask) * 100 << "%\n";
    c.check(areaError(partMask) < 0.01, "固定阈值 120：面积误差小于 1%");

    // 阈值在“谷”里取多少都差不多
    cv::Mat at100, at140;
    cv::threshold(part, at100, 100, 255, cv::THRESH_BINARY_INV);
    cv::threshold(part, at140, 140, 255, cv::THRESH_BINARY_INV);
    std::cout << "阈值 100：" << cv::countNonZero(at100) << "；阈值 140：" << cv::countNonZero(at140) << "\n";
    c.check(std::abs(cv::countNonZero(at100) - cv::countNonZero(at140)) < 600, "阈值取 100 或 140，面积相差不到 1%");

    // ---------- 2. 光线变暗，固定阈值失效 ----------
    const cv::Mat darker = part * 0.6;
    cv::Mat darkerFixed;
    cv::threshold(darker, darkerFixed, 120, 255, cv::THRESH_BINARY_INV);
    cvtut::save(out / "03_darker_fixed_120.png", darkerFixed);
    std::cout << "光线变暗后仍用阈值 120：" << cv::countNonZero(darkerFixed) << " 个像素被当成零件（整张图共 " << part.total()
              << "）\n";
    c.check(cv::countNonZero(darkerFixed) > static_cast<int>(part.total()) * 0.99, "背景也低于 120 了：几乎整张图都被当成零件");

    // ---------- 3. 大津法：自动找阈值 ----------
    cv::Mat otsu;
    const double otsuValue = cv::threshold(part, otsu, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    cv::Mat darkerOtsu;
    const double darkerOtsuValue = cv::threshold(darker, darkerOtsu, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    cvtut::save(out / "03_darker_otsu.png", darkerOtsu);
    std::cout << "大津法：原图选了阈值 " << otsuValue << "，变暗的图选了阈值 " << darkerOtsuValue << "\n";
    c.check(otsuValue > 75 && otsuValue < 165, "原图：阈值落在两个峰（70 和 170）之间");
    c.check(areaError(darkerOtsu) < 0.01, "变暗的图：阈值自动跟着变，面积误差仍小于 1%");

    // ---------- 4. 大津法的问题：图像里只有一种东西时，它照样会“分”出两类 ----------
    cv::Mat blank(480, 640, CV_8UC1, cv::Scalar(170));   // 一张什么都没有的图，只有噪声
    cvtut::addNoise(blank, 3.0);
    cv::Mat blankOtsu;
    const double blankValue = cv::threshold(blank, blankOtsu, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    cvtut::save(out / "03_blank_otsu.png", blankOtsu);
    const double blankRatio = 1.0 * cv::countNonZero(blankOtsu) / static_cast<double>(blank.total());
    std::cout << "空白图像上的大津法：阈值 " << blankValue << "，" << blankRatio * 100 << "% 的像素被分成“暗的一类”\n";
    c.check(blankRatio > 0.2 && blankRatio < 0.8, "空白图像被从噪声中间一分为二");

    // 换成固定阈值：没有东西就是没有东西
    cv::Mat blankFixed;
    cv::threshold(blank, blankFixed, 120, 255, cv::THRESH_BINARY_INV);
    c.check(cv::countNonZero(blankFixed) == 0, "固定阈值：空白图像上一个像素也没有分出来");

    // ---------- 5. 光照不均匀：任何一个全局阈值都不行 ----------
    const cv::Mat dots = cvtut::makeDotsImage();
    const cv::Mat shadedDots = cvtut::applyShading(dots, 1.0, 0.35);
    cvtut::save(out / "03_dots_shaded.png", shadedDots);
    const double dotsArea = 24 * 3.14159265 * 8 * 8;     // 24 个半径 8 的圆点

    cv::Mat dotsOtsu;
    cv::threshold(shadedDots, dotsOtsu, 0, 255, cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
    cvtut::save(out / "03_dots_otsu.png", dotsOtsu);
    std::cout << "不均匀光照下的大津法：分出 " << cv::countNonZero(dotsOtsu) << " 个像素，圆点实际共 " << dotsArea << "\n";
    c.check(cv::countNonZero(dotsOtsu) > dotsArea * 5, "大津法：右边一大片暗的背景也被分了进来");

    // ---------- 6. 自适应阈值：每个像素和它周围的平均亮度比 ----------
    cv::Mat dotsAdaptive;
    // 51：周围 51×51 的范围；10：比周围的平均值暗 10 以上才算
    cv::adaptiveThreshold(shadedDots, dotsAdaptive, 255, cv::ADAPTIVE_THRESH_MEAN_C, cv::THRESH_BINARY_INV, 51, 10);
    cvtut::save(out / "03_dots_adaptive.png", dotsAdaptive);
    const double adaptiveError = std::abs(cv::countNonZero(dotsAdaptive) - dotsArea) / dotsArea;
    std::cout << "自适应阈值：分出 " << cv::countNonZero(dotsAdaptive) << " 个像素，和实际相差 " << adaptiveError * 100 << "%\n";
    c.check(adaptiveError < 0.10, "自适应阈值：面积误差小于 10%");

    // ---------- 7. 自适应阈值的问题：比窗口大的物体会被“掏空” ----------
    cv::Mat partAdaptive;
    cv::adaptiveThreshold(part, partAdaptive, 255, cv::ADAPTIVE_THRESH_MEAN_C, cv::THRESH_BINARY_INV, 51, 10);
    cvtut::save(out / "03_part_adaptive_hollow.png", partAdaptive);
    std::cout << "对大零件用同样的自适应阈值：只分出 " << cv::countNonZero(partAdaptive) << " 个像素，零件实际 " << kPartArea << "\n";
    c.check(cv::countNonZero(partAdaptive) < kPartArea * 0.5, "零件内部和它周围一样暗，不算“比周围暗”：只剩下边缘一圈");
    c.check(partAdaptive.at<uchar>(330, 320) == 0, "零件正中间的像素没有被分出来");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
