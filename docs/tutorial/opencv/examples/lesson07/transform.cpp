// 第 7 课：几何变换（一）。缩放、平移、旋转：像素的位置变了，像素值要靠插值算出来。
#include <cmath>

#include "cvtut.h"

const double kPi = 3.14159265358979;

// 第 5 课的办法：用矩算出图像里白色区域的方向（度）。屏幕上顺时针为正。
double orientation(const cv::Mat& mask) {
    const cv::Moments m = cv::moments(mask, true);
    return 0.5 * std::atan2(2 * m.mu11, m.mu20 - m.mu02) * 180 / kPi;
}

cv::Point2d centroid(const cv::Mat& mask) {
    const cv::Moments m = cv::moments(mask, true);
    return {m.m10 / m.m00, m.m01 / m.m00};
}

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // ---------- 1. 缩小：不同的插值方法，结果差别很大 ----------
    // 一张黑白相间的竖条纹图：每 2 个像素一条（黑 2、白 2、黑 2、白 2……）
    cv::Mat stripes(200, 400, CV_8UC1);
    for (int y = 0; y < stripes.rows; ++y)
        for (int x = 0; x < stripes.cols; ++x) stripes.at<uchar>(y, x) = (x / 2) % 2 == 0 ? 0 : 255;
    cvtut::save(out / "07_stripes.png", stripes);

    cv::Mat byNearest, byArea;
    cv::resize(stripes, byNearest, cv::Size(100, 50), 0, 0, cv::INTER_NEAREST);   // 缩小到 1/4：每 4 个像素里挑 1 个
    cv::resize(stripes, byArea, cv::Size(100, 50), 0, 0, cv::INTER_AREA);         // 缩小到 1/4：每 4×4 个像素取平均
    cvtut::save(out / "07_stripes_nearest.png", byNearest);
    cvtut::save(out / "07_stripes_area.png", byArea);
    cv::Scalar meanNearest, sdNearest, meanArea, sdArea;
    cv::meanStdDev(byNearest, meanNearest, sdNearest);
    cv::meanStdDev(byArea, meanArea, sdArea);
    std::cout << "条纹图缩小到 1/4：最近邻得到的平均灰度 " << meanNearest[0] << "，区域平均得到 " << meanArea[0] << "（原图平均 127.5）\n";
    c.check(meanNearest[0] < 1 || meanNearest[0] > 254, "最近邻：每 4 个像素挑 1 个，正好每次都挑到同一种颜色，整张图变成了纯色");
    c.check(std::abs(meanArea[0] - 127.5) < 1 && sdArea[0] < 1, "区域平均：得到均匀的中间灰色，这才是“缩小后看起来的样子”");

    // ---------- 2. 放大：最近邻有锯齿，线性插值平滑 ----------
    cv::Mat small(8, 8, CV_8UC1, cv::Scalar(0));
    cv::circle(small, cv::Point(4, 4), 3, cv::Scalar(255), cv::FILLED);
    cv::Mat bigNearest, bigLinear;
    cv::resize(small, bigNearest, cv::Size(160, 160), 0, 0, cv::INTER_NEAREST);
    cv::resize(small, bigLinear, cv::Size(160, 160), 0, 0, cv::INTER_LINEAR);
    cvtut::save(out / "07_enlarge_nearest.png", bigNearest);
    cvtut::save(out / "07_enlarge_linear.png", bigLinear);
    cv::Mat middleNearest = (bigNearest > 0) & (bigNearest < 255), middleLinear = (bigLinear > 0) & (bigLinear < 255);
    c.check(cv::countNonZero(middleNearest) == 0, "最近邻放大：只有 0 和 255，没有中间的灰度（一格一格的方块）");
    c.check(cv::countNonZero(middleLinear) > 1000, "线性插值放大：边缘上有很多中间灰度（平滑过渡）");

    // ---------- 3. 平移：用一个 2×3 的矩阵描述 ----------
    cv::Mat dot(200, 300, CV_8UC1, cv::Scalar(0));
    cv::circle(dot, cv::Point(100, 80), 20, cv::Scalar(255), cv::FILLED);
    //   新 x = 1·x + 0·y + 30.5
    //   新 y = 0·x + 1·y + 12
    const cv::Matx23d shift(1, 0, 30.5,    // Matx23d：2 行 3 列的 double 小矩阵，按行依次写出 6 个数
                            0, 1, 12);
    cv::Mat shifted;
    cv::warpAffine(dot, shifted, shift, dot.size());
    const cv::Point2d before = centroid(dot > 127), after = centroid(shifted > 127);
    cv::Moments weighted = cv::moments(shifted, false);   // 按灰度加权的重心：能反映半个像素的移动
    std::cout << "平移 (30.5, 12)：重心从 (" << before.x << ", " << before.y << ") 移到 (" << weighted.m10 / weighted.m00 << ", "
              << weighted.m01 / weighted.m00 << ")\n";
    c.check(std::abs(weighted.m10 / weighted.m00 - before.x - 30.5) < 0.05 && std::abs(weighted.m01 / weighted.m00 - before.y - 12) < 0.05,
            "重心正好移动了 (30.5, 12)：可以平移半个像素");
    (void)after;

    // ---------- 4. 旋转 ----------
    cv::Mat bar(300, 400, CV_8UC1, cv::Scalar(0));
    cv::rectangle(bar, cv::Rect(100, 130, 200, 40), cv::Scalar(255), cv::FILLED);   // 一根水平的长条，中心 (200, 150)
    cv::circle(bar, cv::Point(280, 150), 8, cv::Scalar(0), cv::FILLED);             // 右端有一个黑色的小孔：区分头尾
    cvtut::save(out / "07_bar.png", bar);

    const cv::Point2f center(200, 150);
    const cv::Mat rotation = cv::getRotationMatrix2D(center, 30, 1.0);   // 绕 center 转 30 度，不缩放
    cv::Mat rotated;
    cv::warpAffine(bar, rotated, rotation, bar.size());
    cvtut::save(out / "07_bar_rotated_30.png", rotated);

    const double measured = orientation(rotated > 127);
    std::cout << "getRotationMatrix2D 的角度给 +30：用第 5 课的方法量出来的方向是 " << measured << " 度\n";
    c.check(std::abs(measured - (-30)) < 0.5, "量出来是 -30 度：getRotationMatrix2D 的正角度是【逆时针】，和第 5 课的约定相反");

    // 一个点转到哪里去了：新坐标 = 矩阵 × (x, y, 1)
    const std::vector<cv::Point2f> holeBefore = {{280, 150}};
    std::vector<cv::Point2f> holeAfter;
    cv::transform(holeBefore, holeAfter, rotation);
    std::cout << "小孔原来在 (280, 150)，按矩阵算出旋转后在 (" << holeAfter[0].x << ", " << holeAfter[0].y << ")\n";
    c.check(rotated.at<uchar>(cvRound(holeAfter[0].y), cvRound(holeAfter[0].x)) < 50, "旋转后的图像上，算出来的位置确实是那个黑色的小孔");
    c.check(holeAfter[0].y < 150, "小孔跑到了上方：逆时针转，右端向上抬");

    // 反过来：旋转后的图像上的一个点，对应原图的哪里
    cv::Mat inverse;
    cv::invertAffineTransform(rotation, inverse);
    std::vector<cv::Point2f> backAgain;
    cv::transform(holeAfter, backAgain, inverse);
    c.check(cv::norm(backAgain[0] - holeBefore[0]) < 0.01, "用逆变换可以从旋转后的坐标算回原图的坐标");

    // ---------- 5. 旋转会把角切掉：要把画布放大 ----------
    const cv::Mat part = cvtut::makePartImage();
    cv::Mat partMask;
    cv::threshold(part, partMask, 120, 255, cv::THRESH_BINARY);   // 这里取亮的部分：背景占满了图像的四个角
    cv::Mat clipped;
    const cv::Point2f partCenter(part.cols / 2.0f, part.rows / 2.0f);
    cv::warpAffine(part, clipped, cv::getRotationMatrix2D(partCenter, 30, 1.0), part.size());
    cvtut::save(out / "07_part_rotated_clipped.png", clipped);

    // 旋转之后能把整张图装下的画布大小
    const double angle = 30 * kPi / 180;
    const int newWidth = static_cast<int>(std::ceil(part.cols * std::abs(std::cos(angle)) + part.rows * std::abs(std::sin(angle))));
    const int newHeight = static_cast<int>(std::ceil(part.cols * std::abs(std::sin(angle)) + part.rows * std::abs(std::cos(angle))));
    cv::Mat full = cv::getRotationMatrix2D(partCenter, 30, 1.0);
    full.at<double>(0, 2) += (newWidth - part.cols) / 2.0;    // 再平移一下，让图像落在新画布的中间
    full.at<double>(1, 2) += (newHeight - part.rows) / 2.0;
    cv::Mat whole;
    cv::warpAffine(part, whole, full, cv::Size(newWidth, newHeight));
    cvtut::save(out / "07_part_rotated_whole.png", whole);
    std::cout << "640×480 的图像转 30 度：完整装下需要 " << newWidth << "×" << newHeight << " 的画布\n";

    // 用一张全白的图做同样的变换，得到“哪些像素是真的，哪些是填充的”
    cv::Mat validClipped, validWhole;
    const cv::Mat white(part.size(), CV_8UC1, cv::Scalar(255));
    cv::warpAffine(white, validClipped, cv::getRotationMatrix2D(partCenter, 30, 1.0), part.size(), cv::INTER_NEAREST);
    cv::warpAffine(white, validWhole, full, cv::Size(newWidth, newHeight), cv::INTER_NEAREST);
    cvtut::save(out / "07_valid_mask.png", validWhole);
    const double keptClipped = 1.0 * cv::countNonZero(validClipped) / static_cast<double>(part.total());
    const double keptWhole = 1.0 * cv::countNonZero(validWhole) / static_cast<double>(part.total());
    std::cout << "画布不变：原图只有 " << keptClipped * 100 << "% 的像素留在画面里；放大画布：" << keptWhole * 100 << "%\n";
    c.check(keptClipped < 0.9, "画布不变：四个角被切掉了");
    c.check(std::abs(keptWhole - 1.0) < 0.01, "放大画布：整张图都在");
    c.check(whole.at<uchar>(2, 2) == 0 && validWhole.at<uchar>(2, 2) == 0, "新画布的角上是填充的黑色，掩膜里对应的位置是 0");

    // ---------- 6. 每变换一次，图像就模糊一点 ----------
    cv::Mat once = part.clone(), many = part.clone();
    for (int i = 0; i < 36; ++i) {   // 每次转 10 度，转 36 次，回到原位
        cv::Mat next;
        cv::warpAffine(many, next, cv::getRotationMatrix2D(partCenter, 10, 1.0), many.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        many = next;
    }
    cvtut::save(out / "07_rotated_36_times.png", many);
    // 看划痕（2 个像素宽的亮线）还剩多少对比度：取划痕附近一小块的最大值
    double maxOnce = 0, maxMany = 0;
    const cv::Rect scratch(285, 145, 70, 40);
    cv::minMaxLoc(once(scratch), nullptr, &maxOnce);
    cv::minMaxLoc(many(scratch), nullptr, &maxMany);
    std::cout << "划痕最亮的像素：原图 " << maxOnce << "，连续旋转 36 次之后 " << maxMany << "（零件本身是 70）\n";
    c.check(maxMany < maxOnce - 60, "连续旋转 36 次：细的划痕被抹淡了很多");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
