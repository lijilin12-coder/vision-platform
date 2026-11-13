// 第 5 课：Blob 分析（二）。轮廓、形状特征、方向；按特征把不同的形状分开。
#include <cmath>

#include "cvtut.h"

const double kPi = 3.14159265358979;

// 画一个旋转的实心矩形。angle 是角度（度）：在屏幕上看，正的角度是顺时针转。
void drawRotatedRect(cv::Mat& image, cv::Point2f center, cv::Size2f size, float angle) {
    cv::Point2f corners[4];
    cv::RotatedRect(center, size, angle).points(corners);
    std::vector<cv::Point> polygon;
    for (const auto& p : corners) polygon.emplace_back(cvRound(p.x), cvRound(p.y));
    cv::fillConvexPoly(image, polygon, cv::Scalar(255));
}

// 圆度：4π × 面积 ÷ 周长²。圆是 1，越不圆越小。
double circularity(const std::vector<cv::Point>& contour) {
    const double area = cv::contourArea(contour), perimeter = cv::arcLength(contour, true);
    return perimeter > 0 ? 4 * kPi * area / (perimeter * perimeter) : 0;
}

// 用“矩”计算方向：这一块像素分布得最长的那个方向和 x 轴的夹角（度），范围 (-90, 90]
double orientationByMoments(const std::vector<cv::Point>& contour) {
    const cv::Moments m = cv::moments(contour);
    return 0.5 * std::atan2(2 * m.mu11, m.mu20 - m.mu02) * 180 / kPi;
}

// 用最小外接矩形计算方向：长边和 x 轴的夹角（度），范围 (-90, 90]
double orientationByMinRect(const std::vector<cv::Point>& contour) {
    cv::Point2f p[4];
    cv::minAreaRect(contour).points(p);
    const cv::Point2f e1 = p[1] - p[0], e2 = p[2] - p[1];
    const cv::Point2f longEdge = cv::norm(e1) >= cv::norm(e2) ? e1 : e2;
    double angle = std::atan2(longEdge.y, longEdge.x) * 180 / kPi;
    if (angle > 90) angle -= 180;
    if (angle <= -90) angle += 180;
    return angle;
}

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // ---------- 1. 轮廓：一块区域的边界上的点 ----------
    cv::Mat rect(200, 300, CV_8UC1, cv::Scalar(0));
    cv::rectangle(rect, cv::Rect(50, 40, 200, 100), cv::Scalar(255), cv::FILLED);   // 200×100 的矩形

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(rect, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    c.check(contours.size() == 1, "一个矩形，一条轮廓");
    c.check(contours[0].size() == 4, "CHAIN_APPROX_SIMPLE：直线段只保留两端的点，矩形只需要 4 个点");

    // 轮廓的面积和像素个数不一样
    const double contourArea = cv::contourArea(contours[0]);
    std::cout << "200×100 的矩形：像素个数 " << cv::countNonZero(rect) << "，轮廓面积 " << contourArea << "，周长 "
              << cv::arcLength(contours[0], true) << "\n";
    c.check(cv::countNonZero(rect) == 200 * 100, "数像素：200 × 100 = 20000");
    c.check(contourArea == 199.0 * 99.0, "轮廓面积：199 × 99 = 19701。轮廓上的点在像素的中心，四周各少了半个像素");

    // ---------- 2. 外轮廓和孔 ----------
    const cv::Mat part = cvtut::makePartImage();
    cv::Mat partMask;
    cv::threshold(part, partMask, 120, 255, cv::THRESH_BINARY_INV);

    std::vector<std::vector<cv::Point>> outer, all;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(partMask, outer, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);          // 只要最外面的
    cv::findContours(partMask, all, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE);    // 外轮廓和孔都要
    int holes = 0;
    for (const auto& h : hierarchy) if (h[3] >= 0) ++holes;   // h[3] 是“父轮廓”的编号：有父轮廓的就是孔
    std::cout << "零件：RETR_EXTERNAL 找到 " << outer.size() << " 条轮廓；RETR_CCOMP 找到 " << all.size() << " 条，其中 " << holes
              << " 条是孔\n";
    c.check(outer.size() == 1 && all.size() == 4 && holes == 3, "1 条外轮廓，3 个孔：两个圆孔，加上那道划痕");

    cv::Mat drawn(part.size(), CV_8UC1, cv::Scalar(0));
    cv::drawContours(drawn, all, -1, cv::Scalar(255), 2);
    cvtut::save(out / "05_part_contours.png", drawn);

    // ---------- 3. 一张有各种形状的图像 ----------
    cv::Mat scene(480, 640, CV_8UC1, cv::Scalar(0));
    cv::circle(scene, cv::Point(100, 100), 40, cv::Scalar(255), cv::FILLED);         // 圆
    cv::circle(scene, cv::Point(520, 380), 40, cv::Scalar(255), cv::FILLED);         // 圆
    cv::rectangle(scene, cv::Rect(260, 60, 80, 80), cv::Scalar(255), cv::FILLED);    // 正方形
    drawRotatedRect(scene, {480, 120}, {160, 40}, 30);                               // 长条，转了 30 度
    drawRotatedRect(scene, {140, 340}, {160, 40}, -50);                              // 长条，转了 -50 度
    drawRotatedRect(scene, {330, 300}, {120, 60}, 75);                               // 矩形，转了 75 度
    cvtut::save(out / "05_scene.png", scene);

    cv::findContours(scene, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);     // 保留边界上的每一个点
    c.check(contours.size() == 6, "6 个形状，6 条轮廓");

    // ---------- 4. 形状特征：圆度、长宽比 ----------
    int circles = 0, bars = 0;
    cv::Mat marked;
    cv::cvtColor(scene, marked, cv::COLOR_GRAY2BGR);
    std::cout << "每个形状的特征：\n";
    for (const auto& contour : contours) {
        const cv::RotatedRect box = cv::minAreaRect(contour);    // 面积最小的外接矩形（可以是斜的）
        const double longSide = std::max(box.size.width, box.size.height), shortSide = std::min(box.size.width, box.size.height);
        const double aspect = longSide / shortSide;
        const double round = circularity(contour);
        std::cout << "  中心 (" << cvRound(box.center.x) << ", " << cvRound(box.center.y) << ")  面积 " << cv::contourArea(contour)
                  << "  圆度 " << round << "  长宽比 " << aspect << "\n";
        if (round > 0.85) {
            ++circles;
            cv::circle(marked, box.center, 6, cv::Scalar(0, 0, 255), cv::FILLED);    // 圆：画一个红点
        }
        if (aspect > 3) {
            ++bars;
            cv::Point2f p[4];
            box.points(p);
            for (int i = 0; i < 4; ++i) cv::line(marked, p[i], p[(i + 1) % 4], cv::Scalar(0, 200, 0), 2);   // 长条：画绿框
        }
    }
    cvtut::save(out / "05_scene_marked.png", marked);
    c.check(circles == 2, "圆度大于 0.85 的有 2 个：正好是两个圆");
    c.check(bars == 2, "长宽比大于 3 的有 2 个：正好是两根长条");

    // ---------- 5. 方向 ----------
    // 找到中心在 (480, 120) 附近的那根长条：画的时候转了 30 度
    const auto findNear = [&](cv::Point2f where) -> const std::vector<cv::Point>& {
        const std::vector<cv::Point>* best = &contours[0];
        double bestDistance = 1e9;
        for (const auto& contour : contours) {
            const double d = cv::norm(cv::minAreaRect(contour).center - where);
            if (d < bestDistance) bestDistance = d, best = &contour;
        }
        return *best;
    };
    const struct { cv::Point2f center; double angle; const char* name; } expected[] = {
        {{480, 120}, 30, "长条（30 度）"}, {{140, 340}, -50, "长条（-50 度）"}, {{330, 300}, 75, "矩形（75 度）"}};
    for (const auto& e : expected) {
        const auto& contour = findNear(e.center);
        const double byMoments = orientationByMoments(contour), byRect = orientationByMinRect(contour);
        std::cout << "  " << e.name << "：用矩算出 " << byMoments << " 度，用最小外接矩形算出 " << byRect << " 度\n";
        c.check(std::abs(byMoments - e.angle) < 1.0 && std::abs(byRect - e.angle) < 1.0, std::string(e.name) + "：两种方法的误差都小于 1 度");
    }

    // ---------- 6. 方向的两个局限 ----------
    // (1) 转 180 度之后形状完全一样：分不出“头”和“尾”
    cv::Mat a(200, 300, CV_8UC1, cv::Scalar(0)), b(200, 300, CV_8UC1, cv::Scalar(0));
    drawRotatedRect(a, {150, 100}, {160, 40}, 30);
    drawRotatedRect(b, {150, 100}, {160, 40}, 210);   // 多转 180 度
    cv::Mat different;
    cv::absdiff(a, b, different);
    std::cout << "  长条转 30 度和转 210 度：两张图有 " << cv::countNonZero(different) << " 个像素不同\n";
    c.check(cv::countNonZero(different) <= 10, "长条转 30 度和转 210 度，画出来的图像（几乎）完全相同");
    std::vector<std::vector<cv::Point>> ca, cb;
    cv::findContours(a, ca, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
    cv::findContours(b, cb, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
    c.check(std::abs(orientationByMoments(ca[0]) - orientationByMoments(cb[0])) < 0.5, "算出来的方向也相同：都是 30 度左右");

    // (2) 圆和正方形没有“最长的方向”
    const auto& square = findNear({300, 100});
    const cv::Moments m = cv::moments(square);
    std::cout << "  正方形：mu20 = " << m.mu20 << "，mu02 = " << m.mu02 << "，mu11 = " << m.mu11 << "\n";
    c.check(std::abs(m.mu20 - m.mu02) < 1e-6 * m.mu20 && std::abs(m.mu11) < 1e-6 * m.mu20,
            "正方形在各个方向上“一样长”：算方向的公式变成 atan2(0, 0)，结果没有意义");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
