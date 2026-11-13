// 第 5 课：Blob 分析（一）。把二值图像里互相连着的白色像素归成一块一块，量出每一块的面积、位置、外接框。
#include <cmath>

#include "cvtut.h"

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // ---------- 1. 一张有 24 个圆点的图像：分割，再找连通块 ----------
    const cv::Mat dots = cvtut::makeDotsImage();   // 6 列 4 行，圆点半径 8，中心在 (95 + 90×列, 105 + 90×行)
    cv::Mat mask;
    cv::threshold(dots, mask, 130, 255, cv::THRESH_BINARY_INV);
    cvtut::save(out / "05_dots_mask.png", mask);

    cv::Mat labels, stats, centroids;
    const int n = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8);
    std::cout << "标签个数 " << n << "：第 0 个是背景，所以有 " << n - 1 << " 个连通块\n";
    c.check(n - 1 == 24, "找到 24 个连通块");

    // labels：和原图一样大，每个像素的值是它属于第几块（0 是背景）
    c.check(labels.type() == CV_32SC1, "标签图像的类型是 CV_32SC1：每个像素是一个 int");
    const int labelAtFirstDot = labels.at<int>(105, 95);
    c.check(labelAtFirstDot > 0 && labels.at<int>(10, 10) == 0, "圆点上的像素标签大于 0，背景上的是 0");

    // stats：每一块一行，5 个数：外接框的 左、上、宽、高，以及面积
    // centroids：每一块一行，2 个数：重心的 x、y（是小数）
    const int area = stats.at<int>(labelAtFirstDot, cv::CC_STAT_AREA);
    const int left = stats.at<int>(labelAtFirstDot, cv::CC_STAT_LEFT), top = stats.at<int>(labelAtFirstDot, cv::CC_STAT_TOP);
    const int width = stats.at<int>(labelAtFirstDot, cv::CC_STAT_WIDTH), height = stats.at<int>(labelAtFirstDot, cv::CC_STAT_HEIGHT);
    const double cx = centroids.at<double>(labelAtFirstDot, 0), cy = centroids.at<double>(labelAtFirstDot, 1);
    std::cout << "第一个圆点：面积 " << area << "（半径 8 的圆约 " << 3.14159265 * 64 << "），外接框 左 " << left << " 上 " << top
              << " 宽 " << width << " 高 " << height << "，重心 (" << cx << ", " << cy << ")\n";
    c.check(std::abs(area - 3.14159265 * 64) < 15, "面积和半径 8 的圆相符");
    c.check(width == 17 && height == 17, "外接框 17×17：直径 16 再加中心的那一个像素");
    c.check(std::abs(cx - 95) < 0.2 && std::abs(cy - 105) < 0.2, "重心和画图时的圆心相差不到 0.2 个像素");

    // 所有圆点的重心都应该落在网格上
    double worst = 0;
    for (int i = 1; i < n; ++i) {
        const double x = centroids.at<double>(i, 0), y = centroids.at<double>(i, 1);
        const double gx = 95 + 90 * std::round((x - 95) / 90), gy = 105 + 90 * std::round((y - 105) / 90);
        worst = std::max(worst, std::hypot(x - gx, y - gy));
    }
    std::cout << "24 个重心离理想网格位置最远 " << worst << " 个像素\n";
    c.check(worst < 0.3, "重心的定位精度远小于 1 个像素");

    // 把每一块涂成不同的灰度，方便看
    cv::Mat colored(mask.size(), CV_8UC1, cv::Scalar(0));
    for (int y = 0; y < labels.rows; ++y)
        for (int x = 0; x < labels.cols; ++x)
            if (const int label = labels.at<int>(y, x); label > 0) colored.at<uchar>(y, x) = static_cast<uchar>(60 + label * 8);
    cvtut::save(out / "05_dots_labels.png", colored);

    // ---------- 2. “连着”的两种定义：4 邻域和 8 邻域 ----------
    cv::Mat corner(60, 60, CV_8UC1, cv::Scalar(0));
    cv::rectangle(corner, cv::Rect(10, 10, 20, 20), cv::Scalar(255), cv::FILLED);
    cv::rectangle(corner, cv::Rect(30, 30, 20, 20), cv::Scalar(255), cv::FILLED);   // 两个方块只在一个角上相碰
    cvtut::save(out / "05_corner_touch.png", corner);
    cv::Mat tmp;
    const int with8 = cv::connectedComponents(corner, tmp, 8) - 1;
    const int with4 = cv::connectedComponents(corner, tmp, 4) - 1;
    std::cout << "两个只在角上相碰的方块：8 邻域算 " << with8 << " 块，4 邻域算 " << with4 << " 块\n";
    c.check(with8 == 1 && with4 == 2, "8 邻域：斜着挨着也算连着；4 邻域：只有上下左右挨着才算");

    // ---------- 3. 按面积筛选：去掉噪声造成的小块 ----------
    cv::Mat noisyDots = cvtut::makeDotsImage();
    cvtut::addNoise(noisyDots, 25.0, 3);
    cv::Mat noisyMask;
    cv::threshold(noisyDots, noisyMask, 130, 255, cv::THRESH_BINARY_INV);
    cvtut::save(out / "05_noisy_mask.png", noisyMask);
    const int total = cv::connectedComponentsWithStats(noisyMask, labels, stats, centroids, 8) - 1;
    int kept = 0;
    for (int i = 1; i <= total; ++i) {
        const int a = stats.at<int>(i, cv::CC_STAT_AREA);
        if (a >= 100 && a <= 400) ++kept;   // 圆点的面积约 200
    }
    std::cout << "噪声很大的图像：共 " << total << " 块，面积在 100～400 之间的有 " << kept << " 块\n";
    c.check(total > 50, "噪声产生了很多小块");
    c.check(kept == 24, "按面积筛选之后，正好剩下 24 个圆点");

    // ---------- 4. Blob 分析的局限：挨在一起的物体会被当成一个 ----------
    cv::Mat touching(120, 240, CV_8UC1, cv::Scalar(0));
    cv::circle(touching, cv::Point(90, 60), 30, cv::Scalar(255), cv::FILLED);
    cv::circle(touching, cv::Point(145, 60), 30, cv::Scalar(255), cv::FILLED);     // 圆心相距 55，小于两个半径之和 60：有重叠
    cvtut::save(out / "05_touching.png", touching);
    const int touchingCount = cv::connectedComponentsWithStats(touching, labels, stats, centroids, 8) - 1;
    std::cout << "两个有一点重叠的圆：找到 " << touchingCount << " 块，面积 " << stats.at<int>(1, cv::CC_STAT_AREA) << "（一个圆约 "
              << 3.14159265 * 900 << "），重心 x = " << centroids.at<double>(1, 0) << "\n";
    c.check(touchingCount == 1, "两个圆被当成了一块");
    c.check(std::abs(centroids.at<double>(1, 0) - 117.5) < 0.5, "重心落在两个圆中间：那里其实哪个圆的圆心都不是");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
