// 第 2 课：像素的数据类型。8 位的溢出与饱和、转成浮点数计算、16 位图像。
#include "cvtut.h"

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // ---------- 1. 8 位像素只能表示 0～255：超出了怎么办 ----------
    const uchar a = 200, b = 100;
    const uchar wrapped = static_cast<uchar>(a + b);         // 300 放不下，只留下低 8 位：300 - 256 = 44
    const uchar saturated = cv::saturate_cast<uchar>(a + b); // 截断到 255
    std::cout << "200 + 100：直接转成 uchar 得到 " << int(wrapped) << "，saturate_cast 得到 " << int(saturated) << "\n";
    c.check(wrapped == 44, "直接转换：溢出后“绕回去”，亮的地方反而变暗了");
    c.check(saturated == 255, "saturate_cast：超过 255 就取 255，低于 0 就取 0");
    c.check(cv::saturate_cast<uchar>(50 - 80) == 0, "50 - 80：saturate_cast 得到 0");

    // ---------- 2. 整张图像调亮：手写和用 OpenCV ----------
    const cv::Mat part = cvtut::makePartImage();
    cvtut::save(out / "02_part.png", part);

    cv::Mat wrong = part.clone();
    for (int y = 0; y < wrong.rows; ++y) {
        uchar* row = wrong.ptr<uchar>(y);
        for (int x = 0; x < wrong.cols; ++x) row[x] = static_cast<uchar>(row[x] + 100);  // 错误的写法
    }
    cvtut::save(out / "02_brighter_wrong.png", wrong);   // 原来亮的背景变成了很暗的颜色

    const cv::Mat right = part + 100;                    // OpenCV 的运算自带饱和处理
    cvtut::save(out / "02_brighter_right.png", right);
    const cv::Point background(50, 50);                  // 背景上的一个点，原来约 170
    std::cout << "背景像素原来是 " << int(part.at<uchar>(background)) << "：错误写法得到 "
              << int(wrong.at<uchar>(background)) << "，OpenCV 得到 " << int(right.at<uchar>(background)) << "\n";
    c.check(right.at<uchar>(background) == 255 && wrong.at<uchar>(background) < 50, "OpenCV 的加法会饱和，手写的会绕回");

    // ---------- 3. 减法同样会饱和：想知道“相差多少”要用 absdiff ----------
    const cv::Mat darker = part - 30;
    const cv::Mat minus = darker - part;                 // 每个像素都是负数 → 全部变成 0
    cv::Mat diff;
    cv::absdiff(darker, part, diff);                     // 差的绝对值
    c.check(cv::countNonZero(minus) == 0, "暗图减亮图：结果全是 0，差别“消失”了");
    c.check(diff.at<uchar>(background) == 30, "absdiff：得到差的绝对值 30");

    // ---------- 4. 需要小数或负数时：转成浮点数再算 ----------
    cv::Mat asFloat;
    part.convertTo(asFloat, CV_32F, 1.0 / 255.0);        // 转成 32 位浮点数，同时乘以 1/255：范围变成 0～1
    c.check(asFloat.type() == CV_32FC1, "convertTo 之后类型是 CV_32FC1");
    double minValue = 0, maxValue = 0;
    cv::minMaxLoc(asFloat, &minValue, &maxValue);
    std::cout << "转成浮点数之后，最小 " << minValue << "，最大 " << maxValue << "\n";
    c.check(minValue >= 0.0 && maxValue <= 1.0, "数值范围在 0～1 之间");

    // 浮点数可以放心地做会产生小数和负数的计算，算完再转回 8 位
    cv::Mat contrast = (asFloat - 0.5f) * 1.5f + 0.5f;   // 以中间灰度为中心，把对比度拉大 1.5 倍
    cv::Mat contrast8;
    contrast.convertTo(contrast8, CV_8U, 255.0);         // 乘以 255 转回 8 位，自动四舍五入并饱和
    cvtut::save(out / "02_more_contrast.png", contrast8);
    c.check(contrast8.type() == CV_8UC1, "转回 8 位");

    // 用 at 读像素时，类型必须和图像的实际类型一致
    c.check(asFloat.at<float>(background) > 0.6f, "浮点图像用 at<float> 读");
    // asFloat.at<uchar>(background) 可以编译，但读到的是浮点数的某一个字节，毫无意义

    // ---------- 5. 16 位图像：一些工业相机输出 10 位或 12 位的数据 ----------
    cv::Mat image16;
    part.convertTo(image16, CV_16U, 16.0);               // 模拟 12 位相机：数值范围 0～4095
    cv::minMaxLoc(image16, &minValue, &maxValue);
    std::cout << "16 位图像：最小 " << minValue << "，最大 " << maxValue << "\n";
    c.check(image16.type() == CV_16UC1 && maxValue > 255, "CV_16UC1：一个像素占 2 字节，可以超过 255");

    cvtut::save(out / "02_part_16bit.png", image16);     // PNG 支持 16 位
    const cv::Mat back = cvtut::load(out / "02_part_16bit.png", cv::IMREAD_UNCHANGED);
    c.check(back.type() == CV_16UC1, "用 IMREAD_UNCHANGED 读回来，仍然是 16 位");
    const cv::Mat truncated = cvtut::load(out / "02_part_16bit.png", cv::IMREAD_GRAYSCALE);
    c.check(truncated.type() == CV_8UC1, "用 IMREAD_GRAYSCALE 读，被悄悄转成了 8 位");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
