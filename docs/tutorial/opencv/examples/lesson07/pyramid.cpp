// 第 7 课：几何变换（二）。图像金字塔：同一张图像的一组越来越小的版本，用来“先粗看，再细看”。
#include <chrono>
#include <cmath>

#include "cvtut.h"

cv::Point2d centroid(const cv::Mat& mask) {
    const cv::Moments m = cv::moments(mask, true);
    return {m.m10 / m.m00, m.m01 / m.m00};
}

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // ---------- 1. 建立金字塔：每一层的宽和高都是上一层的一半 ----------
    const cv::Mat part = cvtut::makePartImage();
    std::vector<cv::Mat> pyramid = {part};
    for (int level = 1; level <= 3; ++level) {
        cv::Mat next;
        cv::pyrDown(pyramid.back(), next);   // 先高斯滤波，再每隔一个像素取一个
        pyramid.push_back(next);
    }
    std::size_t totalPixels = 0;
    for (std::size_t level = 0; level < pyramid.size(); ++level) {
        std::cout << "第 " << level << " 层：" << pyramid[level].cols << "×" << pyramid[level].rows << "，" << pyramid[level].total() << " 个像素\n";
        cvtut::save(out / ("07_pyramid_level" + std::to_string(level) + ".png"), pyramid[level]);
        totalPixels += pyramid[level].total();
    }
    c.check(pyramid[1].cols == 320 && pyramid[1].rows == 240 && pyramid[3].cols == 80 && pyramid[3].rows == 60, "每一层的宽、高是上一层的一半");
    c.check(pyramid[3].total() * 64 == pyramid[0].total(), "第 3 层的像素数是原图的 1/64");
    c.check(totalPixels < pyramid[0].total() * 4 / 3 + 1, "整个金字塔的像素总数不超过原图的 4/3：多占的内存很少");

    // ---------- 2. 为什么要先滤波再缩小 ----------
    cv::Mat noisy = cvtut::makePartImage();
    cvtut::addNoise(noisy, 20.0, 9);
    cv::Mat skipped(noisy.rows / 2, noisy.cols / 2, CV_8UC1);   // 不滤波，直接每隔一个像素取一个
    for (int y = 0; y < skipped.rows; ++y)
        for (int x = 0; x < skipped.cols; ++x) skipped.at<uchar>(y, x) = noisy.at<uchar>(y * 2, x * 2);
    cv::Mat proper;
    cv::pyrDown(noisy, proper);
    cv::Scalar mean, sdSkipped, sdProper, sdOriginal;
    cv::meanStdDev(noisy(cv::Rect(10, 10, 100, 80)), mean, sdOriginal);
    cv::meanStdDev(skipped(cv::Rect(5, 5, 50, 40)), mean, sdSkipped);
    cv::meanStdDev(proper(cv::Rect(5, 5, 50, 40)), mean, sdProper);
    std::cout << "背景的噪声：原图 " << sdOriginal[0] << "；直接隔一个取一个 " << sdSkipped[0] << "；pyrDown " << sdProper[0] << "\n";
    c.check(std::abs(sdSkipped[0] - sdOriginal[0]) < 3, "直接抽取：噪声和原图一样大");
    c.check(sdProper[0] < sdOriginal[0] / 2, "pyrDown：噪声小了一半以上（缩小的同时做了平均）");

    // ---------- 3. 各层之间的坐标关系 ----------
    // 在第 3 层（80×60）上找零件的重心，换算回原图的坐标：乘以 8
    std::vector<cv::Point2d> centers;
    for (const auto& level : pyramid) {
        cv::Mat mask;
        cv::threshold(level, mask, 120, 255, cv::THRESH_BINARY_INV);
        centers.push_back(centroid(mask));
    }
    const double scale = 8;   // 2 的 3 次方
    const cv::Point2d fromCoarse(centers[3].x * scale, centers[3].y * scale);
    std::cout << "零件的重心：在原图上 (" << centers[0].x << ", " << centers[0].y << ")；在第 3 层上 (" << centers[3].x << ", " << centers[3].y
              << ")，换算回原图是 (" << fromCoarse.x << ", " << fromCoarse.y << ")\n";
    const double error = std::hypot(fromCoarse.x - centers[0].x, fromCoarse.y - centers[0].y);
    std::cout << "两者相差 " << error << " 个像素（第 3 层的 1 个像素相当于原图的 8 个像素）\n";
    c.check(error < 2, "在第 3 层上找到的位置乘以 8，和在原图上找到的位置相差不到 2 个像素");

    // ---------- 4. 小的东西在高层上会消失 ----------
    // 划痕：2 个像素宽的亮线（230），周围是零件（70）
    std::cout << "划痕附近最亮的像素：";
    std::vector<double> scratchMax;
    for (std::size_t level = 0; level < pyramid.size(); ++level) {
        const int f = 1 << level;
        const cv::Rect area(285 / f, 145 / f, 70 / f, 40 / f);
        double maxValue = 0;
        cv::minMaxLoc(pyramid[level](area), nullptr, &maxValue);
        scratchMax.push_back(maxValue);
        std::cout << "第 " << level << " 层 " << maxValue << "  ";
    }
    std::cout << "（零件本身是 70）\n";
    c.check(scratchMax[0] > 200, "原图上划痕很明显");
    c.check(scratchMax[3] - 70 < (scratchMax[0] - 70) / 4, "到第 3 层，划痕比零件只亮一点点了（对比度不到原来的 1/4）：它被平均掉了");
    // 两个圆孔（直径 70）在第 3 层上还剩 9 个像素左右，仍然看得见
    c.check(pyramid[3].at<uchar>(30, 27) > 140, "直径 70 的圆孔在第 3 层上还在");

    // ---------- 5. 在小图上处理快得多 ----------
    const auto timeIt = [](const cv::Mat& image) {
        const auto start = std::chrono::steady_clock::now();
        cv::Mat result;
        for (int i = 0; i < 20; ++i) cv::GaussianBlur(image, result, cv::Size(9, 9), 2.0);
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 20;
    };
    cv::Mat large;
    cv::resize(part, large, cv::Size(2560, 1920));   // 放大成一张 5MP 的图，时间才量得准
    std::vector<cv::Mat> largePyramid = {large};
    for (int level = 1; level <= 3; ++level) {
        cv::Mat next;
        cv::pyrDown(largePyramid.back(), next);
        largePyramid.push_back(next);
    }
    (void)timeIt(largePyramid[0]);   // 预热
    std::cout << "对 5MP 图像的各层做同一个 9×9 高斯滤波的耗时：";
    for (std::size_t level = 0; level < largePyramid.size(); ++level) std::cout << "第 " << level << " 层 " << timeIt(largePyramid[level]) << " 微秒  ";
    std::cout << "\n";

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
