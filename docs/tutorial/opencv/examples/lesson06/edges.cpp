// 第 6 课：边缘（一）。边缘就是亮度变化快的地方：梯度、Sobel、Canny。
#include <cmath>

#include "cvtut.h"

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // 零件图像，稍微模糊一下：真实镜头拍出来的边缘不是“一刀切”的，而是有两三个像素的过渡
    cv::Mat part = cvtut::makePartImage();
    cv::GaussianBlur(part, part, cv::Size(5, 5), 1.0);
    cvtut::save(out / "06_part.png", part);

    // ---------- 1. 沿一条水平线看亮度：穿过零件的左边缘（x = 140 附近） ----------
    const int y = 330;   // 这一行不经过圆孔
    std::cout << "第 " << y << " 行，x = 134～146 的灰度：";
    for (int x = 134; x <= 146; ++x) std::cout << int(part.at<uchar>(y, x)) << " ";
    std::cout << "\n相邻两个像素的差（右减左）：  ";
    int steepestX = 0, steepest = 0;
    for (int x = 134; x < 146; ++x) {
        const int d = part.at<uchar>(y, x + 1) - part.at<uchar>(y, x);
        std::cout << d << " ";
        if (std::abs(d) > std::abs(steepest)) steepest = d, steepestX = x;
    }
    std::cout << "\n";
    c.check(steepest < -20, "边缘处相邻像素的差很大，而且是负的：从亮（背景）变到暗（零件）");
    c.check(std::abs(steepestX - 139) <= 1, "变化最快的位置在 x = 139～140：就是零件的左边缘");

    // ---------- 2. Sobel：对整张图像求“x 方向的变化”和“y 方向的变化” ----------
    cv::Mat dx, dy;
    cv::Sobel(part, dx, CV_16S, 1, 0);   // x 方向的梯度。结果有正有负，所以用 16 位有符号数（第 2 课）
    cv::Sobel(part, dy, CV_16S, 0, 1);   // y 方向的梯度
    const int atLeft = dx.at<short>(y, 140), atRight = dx.at<short>(y, 500), atTop = dy.at<short>(110, 320);
    std::cout << "Sobel：左边缘 dx = " << atLeft << "，右边缘 dx = " << atRight << "，上边缘 dy = " << atTop
              << "，零件内部 dx = " << dx.at<short>(y, 320) << "\n";
    c.check(atLeft < -100 && atRight > 100, "左边缘（亮→暗）dx 是负的，右边缘（暗→亮）dx 是正的");
    c.check(std::abs(dx.at<short>(110, 320)) < 30 && atTop < -100, "上边缘是水平的：dx 接近 0，dy 很大");
    c.check(std::abs(dx.at<short>(y, 320)) < 30, "零件内部亮度均匀：梯度接近 0");

    // 如果输出类型用 8 位无符号数，负的梯度全部变成 0：一半的边缘丢了
    cv::Mat dx8;
    cv::Sobel(part, dx8, CV_8U, 1, 0);
    cvtut::save(out / "06_sobel_dx_8bit_wrong.png", dx8);
    c.check(dx8.at<uchar>(y, 140) == 0 && dx8.at<uchar>(y, 500) > 100, "输出用 CV_8U：左边缘消失了，只剩右边缘");

    // 梯度的大小：不管方向，只看变化有多快
    cv::Mat dxF, dyF, magnitude;
    dx.convertTo(dxF, CV_32F);
    dy.convertTo(dyF, CV_32F);
    cv::magnitude(dxF, dyF, magnitude);   // 每个像素：sqrt(dx² + dy²)
    cv::Mat magnitude8;
    magnitude.convertTo(magnitude8, CV_8U, 0.5);
    cvtut::save(out / "06_gradient_magnitude.png", magnitude8);
    c.check(magnitude.at<float>(y, 140) > 100 && magnitude.at<float>(110, 320) > 100, "梯度的大小：四条边上都很大");

    // 对梯度的大小做阈值，得到的边缘有好几个像素宽
    cv::Mat thick = magnitude > 100;
    cvtut::save(out / "06_gradient_threshold.png", thick);
    int thickness = 0;
    for (int x = 130; x < 150; ++x) thickness += thick.at<uchar>(y, x) ? 1 : 0;
    std::cout << "对梯度大小做阈值：左边缘有 " << thickness << " 个像素宽\n";
    c.check(thickness >= 2, "边缘不是一条细线，而是一条带子");

    // ---------- 3. Canny：得到一个像素宽的边缘 ----------
    cv::Mat edges;
    cv::Canny(part, edges, 50, 150);   // 两个门限：低的和高的
    cvtut::save(out / "06_canny.png", edges);
    int cannyThickness = 0;
    for (int x = 130; x < 150; ++x) cannyThickness += edges.at<uchar>(y, x) ? 1 : 0;
    c.check(cannyThickness == 1, "Canny 的结果：左边缘正好 1 个像素宽");

    // 边缘像素的总数应该和各条边界的总长度差不多
    const double expected = 2 * (360 + 260) + 2 * (2 * 3.14159265 * 35);   // 矩形的周长 + 两个圆孔的周长（划痕另算）
    std::cout << "Canny 找到 " << cv::countNonZero(edges) << " 个边缘像素；矩形加两个圆孔的周长约 " << expected << "\n";
    c.check(cv::countNonZero(edges) > expected * 0.85 && cv::countNonZero(edges) < expected * 1.25, "边缘像素数和边界总长度相符");

    // ---------- 4. 噪声对边缘检测的影响 ----------
    cv::Mat noisy = cvtut::makePartImage();
    cvtut::addNoise(noisy, 12.0, 5);
    cv::Mat noisyEdges, smoothed, smoothedEdges;
    cv::Canny(noisy, noisyEdges, 50, 150);
    cv::GaussianBlur(noisy, smoothed, cv::Size(5, 5), 1.5);
    cv::Canny(smoothed, smoothedEdges, 50, 150);
    cvtut::save(out / "06_canny_noisy.png", noisyEdges);
    cvtut::save(out / "06_canny_noisy_smoothed.png", smoothedEdges);
    std::cout << "噪声较大的图像：直接 Canny 得到 " << cv::countNonZero(noisyEdges) << " 个边缘像素；先高斯滤波再 Canny 得到 "
              << cv::countNonZero(smoothedEdges) << " 个\n";
    c.check(cv::countNonZero(noisyEdges) > expected * 3, "直接做：噪声被当成了大量的“边缘”");
    c.check(cv::countNonZero(smoothedEdges) < expected * 1.3, "先滤波：只剩下真正的边缘");

    // ---------- 5. 光线变暗：边缘的位置不变 ----------
    const cv::Mat darker = part * 0.6;
    int steepestDarkX = 0, steepestDark = 0;
    for (int x = 134; x < 146; ++x) {
        const int d = darker.at<uchar>(y, x + 1) - darker.at<uchar>(y, x);
        if (std::abs(d) > std::abs(steepestDark)) steepestDark = d, steepestDarkX = x;
    }
    std::cout << "光线变暗到 60%：变化最快的位置在 x = " << steepestDarkX << "（原来 " << steepestX << "），差值从 " << steepest
              << " 变成 " << steepestDark << "\n";
    c.check(steepestDarkX == steepestX, "边缘的位置没有变，只是梯度变小了");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
