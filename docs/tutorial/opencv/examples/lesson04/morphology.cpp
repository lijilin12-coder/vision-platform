// 第 4 课：形态学。在二值图像上“修形状”：腐蚀、膨胀、开运算、闭运算。
#include "cvtut.h"

// 数一数二值图像里有几块互不相连的白色区域（下一课详细讲）
int countBlobs(const cv::Mat& mask) {
    cv::Mat labels;
    return cv::connectedComponents(mask, labels, 8) - 1;   // 减去背景
}

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // ---------- 1. 腐蚀和膨胀 ----------
    cv::Mat square(200, 300, CV_8UC1, cv::Scalar(0));
    cv::rectangle(square, cv::Rect(100, 50, 100, 100), cv::Scalar(255), cv::FILLED);   // 100×100 的白色方块

    const cv::Mat k3 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));      // 3×3 的结构元素
    cv::Mat eroded, dilated;
    cv::erode(square, eroded, k3);     // 腐蚀：周围 3×3 里只要有一个黑的，就变黑 → 白色区域缩小一圈
    cv::dilate(square, dilated, k3);   // 膨胀：周围 3×3 里只要有一个白的，就变白 → 白色区域长大一圈
    std::cout << "100×100 的方块：腐蚀后 " << cv::countNonZero(eroded) << " 个像素，膨胀后 " << cv::countNonZero(dilated) << "\n";
    c.check(cv::countNonZero(eroded) == 98 * 98, "3×3 腐蚀：四周各缩小 1 个像素，变成 98×98");
    c.check(cv::countNonZero(dilated) == 102 * 102, "3×3 膨胀：四周各长大 1 个像素，变成 102×102");

    // ---------- 2. 一张有毛病的二值图像 ----------
    // 噪声很大的零件图像，直接阈值分割：背景上有白色的噪声点，零件里有黑色的小洞
    cv::Mat noisy(480, 640, CV_8UC1, cv::Scalar(170));
    cv::rectangle(noisy, cv::Rect(140, 110, 360, 260), cv::Scalar(70), cv::FILLED);
    cv::circle(noisy, cv::Point(220, 240), 35, cv::Scalar(170), cv::FILLED);
    cv::circle(noisy, cv::Point(420, 240), 35, cv::Scalar(170), cv::FILLED);
    cvtut::addNoise(noisy, 22.0);
    cv::Mat mask;
    cv::threshold(noisy, mask, 120, 255, cv::THRESH_BINARY_INV);
    cvtut::save(out / "04_mask_raw.png", mask);
    const double trueArea = 360.0 * 260.0 - 2 * 3.14159265 * 35 * 35;
    std::cout << "直接阈值分割：" << countBlobs(mask) << " 块白色区域（应该只有 1 块），面积 " << cv::countNonZero(mask)
              << "（真实 " << trueArea << "）\n";
    c.check(countBlobs(mask) > 100, "直接分割的结果里有上百块零碎的白色区域");

    // ---------- 3. 开运算：先腐蚀再膨胀 → 去掉小的白点 ----------
    cv::Mat opened;
    cv::morphologyEx(mask, opened, cv::MORPH_OPEN, k3);
    cvtut::save(out / "04_mask_opened.png", opened);
    std::cout << "开运算之后：" << countBlobs(opened) << " 块白色区域\n";
    c.check(countBlobs(opened) < countBlobs(mask) / 20, "开运算去掉了绝大部分小白点");

    // 用两步写出来，结果完全相同
    cv::Mat step1, step2;
    cv::erode(mask, step1, k3);
    cv::dilate(step1, step2, k3);
    c.check(cv::norm(opened, step2, cv::NORM_INF) == 0, "开运算 = 先腐蚀，再膨胀");

    // ---------- 4. 闭运算：先膨胀再腐蚀 → 填上小的黑洞 ----------
    cv::Mat cleaned;
    cv::morphologyEx(opened, cleaned, cv::MORPH_CLOSE, k3);
    cvtut::save(out / "04_mask_cleaned.png", cleaned);
    const cv::Rect insidePart(250, 290, 140, 60);   // 零件内部的一块，本来应该全是白的
    const int holesBefore = insidePart.area() - cv::countNonZero(opened(insidePart));
    const int holesAfter = insidePart.area() - cv::countNonZero(cleaned(insidePart));
    std::cout << "零件内部一块 " << insidePart.area() << " 个像素的区域里：闭运算前有 " << holesBefore << " 个黑点，之后 " << holesAfter << " 个\n";
    c.check(holesAfter < holesBefore / 5, "闭运算填上了大部分小黑洞");

    const double areaError = std::abs(cv::countNonZero(cleaned) - trueArea) / trueArea;
    std::cout << "整理之后：" << countBlobs(cleaned) << " 块，面积 " << cv::countNonZero(cleaned) << "，和真实面积相差 "
              << areaError * 100 << "%\n";
    c.check(areaError < 0.02, "整理之后面积误差小于 2%");
    c.check(cleaned.at<uchar>(240, 220) == 0 && cleaned.at<uchar>(240, 420) == 0, "两个大圆孔还在：闭运算只填比结构元素小的洞");

    // ---------- 5. 结构元素的大小决定“多小算小” ----------
    // 一个大方块、一条 3 像素宽的细线、一个 7×7 的小点
    cv::Mat shapes(200, 400, CV_8UC1, cv::Scalar(0));
    cv::rectangle(shapes, cv::Rect(30, 50, 100, 100), cv::Scalar(255), cv::FILLED);
    cv::rectangle(shapes, cv::Rect(170, 99, 120, 3), cv::Scalar(255), cv::FILLED);
    cv::rectangle(shapes, cv::Rect(330, 97, 7, 7), cv::Scalar(255), cv::FILLED);
    cvtut::save(out / "04_shapes.png", shapes);

    cv::Mat open5, open9;
    cv::morphologyEx(shapes, open5, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));
    cv::morphologyEx(shapes, open9, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(9, 9)));
    cvtut::save(out / "04_shapes_open5.png", open5);
    cvtut::save(out / "04_shapes_open9.png", open9);
    std::cout << "三个形状：5×5 开运算后剩 " << countBlobs(open5) << " 个，9×9 开运算后剩 " << countBlobs(open9) << " 个\n";
    c.check(countBlobs(open5) == 2, "5×5：3 像素宽的细线放不下 5×5，被去掉；7×7 的小点留下");
    c.check(countBlobs(open9) == 1, "9×9：小点也被去掉，只剩大方块");
    c.check(cv::countNonZero(open9) == 100 * 100, "大方块完好无损：开运算不改变大物体的大小");

    // 用原图减去开运算的结果，得到的正好是“被去掉的细小部分”
    cv::Mat thinParts = shapes - open5;
    cvtut::save(out / "04_shapes_thin_parts.png", thinParts);
    c.check(countBlobs(thinParts) == 1 && cv::countNonZero(thinParts) == 120 * 3, "原图 − 开运算 = 那条细线");

    // ---------- 6. 副作用：闭运算会把靠得近的物体连起来 ----------
    cv::Mat two(100, 200, CV_8UC1, cv::Scalar(0));
    cv::rectangle(two, cv::Rect(40, 30, 50, 40), cv::Scalar(255), cv::FILLED);
    cv::rectangle(two, cv::Rect(96, 30, 50, 40), cv::Scalar(255), cv::FILLED);   // 两个方块之间隔 6 个像素
    cv::Mat twoClosed;
    cv::morphologyEx(two, twoClosed, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(9, 9)));
    cvtut::save(out / "04_two_closed.png", twoClosed);
    c.check(countBlobs(two) == 2 && countBlobs(twoClosed) == 1, "相隔 6 个像素的两个方块，9×9 闭运算后连成了一个");

    // ---------- 7. 膨胀 − 腐蚀 = 轮廓 ----------
    cv::Mat outline;
    cv::morphologyEx(cleaned, outline, cv::MORPH_GRADIENT, k3);
    cvtut::save(out / "04_outline.png", outline);
    c.check(cv::countNonZero(outline) < cv::countNonZero(cleaned) / 10, "只剩下边界上的一圈");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
