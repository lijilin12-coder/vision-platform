// 第 2 课：通道与颜色。BGR 顺序、拆分与合并、转灰度、用 HSV 按颜色找东西。
#include <cmath>

#include "cvtut.h"

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // ---------- 1. 彩色图像的一个像素是三个数，顺序是 蓝、绿、红 ----------
    cv::Mat red(100, 100, CV_8UC3, cv::Scalar(0, 0, 255));  // 蓝 0，绿 0，红 255
    const cv::Vec3b pixel = red.at<cv::Vec3b>(0, 0);
    c.check(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255, "纯红色的像素是 (0, 0, 255)：红色排在最后");
    cvtut::save(out / "02_pure_red.png", red);

    const cv::Mat board = cvtut::makeBoardImage();
    cvtut::save(out / "02_board.png", board);
    c.check(board.channels() == 3 && board.type() == CV_8UC3, "彩色图像：3 个通道，类型 CV_8UC3");
    c.check(board.elemSize() == 3 && board.step == 640u * 3u, "一个像素占 3 字节，一行占 640×3 字节");

    // ---------- 2. 拆成三张单通道图像 ----------
    std::vector<cv::Mat> bgr;
    cv::split(board, bgr);  // bgr[0] 蓝，bgr[1] 绿，bgr[2] 红
    cvtut::save(out / "02_channel_blue.png", bgr[0]);
    cvtut::save(out / "02_channel_green.png", bgr[1]);
    cvtut::save(out / "02_channel_red.png", bgr[2]);

    // 红色元件中心的一个像素：红通道里很亮，另外两个通道里很暗
    const cv::Point onRed(155, 125);
    std::cout << "红色元件上的一个像素：蓝 " << int(bgr[0].at<uchar>(onRed)) << "，绿 " << int(bgr[1].at<uchar>(onRed))
              << "，红 " << int(bgr[2].at<uchar>(onRed)) << "\n";
    c.check(bgr[2].at<uchar>(onRed) > 150 && bgr[0].at<uchar>(onRed) < 80, "红色元件在红通道里亮，在蓝通道里暗");

    // ---------- 3. 合并回去；交换通道 ----------
    cv::Mat merged;
    cv::merge(bgr, merged);
    c.check(cv::norm(board, merged, cv::NORM_INF) == 0, "拆开再合并，和原图完全相同");

    cv::Mat swapped;
    cv::cvtColor(board, swapped, cv::COLOR_BGR2RGB);  // 把蓝和红对调
    cvtut::save(out / "02_swapped_wrong_colors.png", swapped);  // 保存时仍按 BGR 解释，所以红蓝颠倒了
    c.check(swapped.at<cv::Vec3b>(onRed)[0] == board.at<cv::Vec3b>(onRed)[2], "BGR2RGB：第 0 个和第 2 个通道对调");

    // ---------- 4. 彩色转灰度：不是三个通道简单平均 ----------
    cv::Mat gray;
    cv::cvtColor(board, gray, cv::COLOR_BGR2GRAY);
    cvtut::save(out / "02_gray.png", gray);
    c.check(gray.channels() == 1, "转灰度之后是 1 个通道");

    const cv::Vec3b px = board.at<cv::Vec3b>(onRed);
    const double byFormula = 0.114 * px[0] + 0.587 * px[1] + 0.299 * px[2];
    std::cout << "灰度值：OpenCV 算出 " << int(gray.at<uchar>(onRed)) << "，按公式 0.299R+0.587G+0.114B 算出 " << byFormula
              << "，简单平均是 " << (px[0] + px[1] + px[2]) / 3.0 << "\n";
    c.check(std::abs(gray.at<uchar>(onRed) - byFormula) <= 1.0, "灰度 = 0.299×红 + 0.587×绿 + 0.114×蓝");

    // ---------- 5. 按颜色找东西：转到 HSV ----------
    cv::Mat hsv;
    cv::cvtColor(board, hsv, cv::COLOR_BGR2HSV);  // H 色相（什么颜色）、S 饱和度（颜色有多纯）、V 亮度
    const cv::Vec3b hsvYellow = hsv.at<cv::Vec3b>(330, 150);
    std::cout << "黄色电容上的一个像素：H " << int(hsvYellow[0]) << "，S " << int(hsvYellow[1]) << "，V "
              << int(hsvYellow[2]) << "（OpenCV 里 8 位图像的 H 范围是 0～179）\n";

    // 黄色：H 在 20～35 之间，并且颜色足够纯、足够亮
    cv::Mat yellowMask;
    cv::inRange(hsv, cv::Scalar(20, 100, 100), cv::Scalar(35, 255, 255), yellowMask);
    cvtut::save(out / "02_mask_yellow.png", yellowMask);
    const int yellowPixels = cv::countNonZero(yellowMask);
    const double twoCircles = 2 * 3.14159 * 30 * 30;
    std::cout << "黄色像素 " << yellowPixels << " 个；两个半径 30 的圆的面积约 " << twoCircles << "\n";
    c.check(std::abs(yellowPixels - twoCircles) < twoCircles * 0.05, "找到的黄色面积和两个电容的面积相符（误差 5% 以内）");

    // 红色比较特殊：色相是一个圆环，红色正好跨在 0 的两边，要分两段找，再合起来
    cv::Mat redLow, redHigh, redMask;
    cv::inRange(hsv, cv::Scalar(0, 100, 100), cv::Scalar(10, 255, 255), redLow);
    cv::inRange(hsv, cv::Scalar(170, 100, 100), cv::Scalar(179, 255, 255), redHigh);
    cv::bitwise_or(redLow, redHigh, redMask);
    cvtut::save(out / "02_mask_red.png", redMask);
    const int redPixels = cv::countNonZero(redMask);
    std::cout << "红色像素 " << redPixels << " 个；两个 90×50 的矩形共 " << 2 * 90 * 50 << "\n";
    c.check(std::abs(redPixels - 9000) < 450, "找到的红色面积和两个红色元件的面积相符（误差 5% 以内）");

    // 只留下黄色的部分，其余涂黑
    cv::Mat onlyYellow;
    board.copyTo(onlyYellow, yellowMask);  // 只复制掩膜里不为 0 的位置
    cvtut::save(out / "02_only_yellow.png", onlyYellow);

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
