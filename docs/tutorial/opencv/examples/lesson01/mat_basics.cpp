// 第 1 课：cv::Mat 是什么。创建图像、读写像素、复制与共享、取出一块区域。
#include "cvtut.h"

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // ---------- 1. 创建一张图像 ----------
    // 480 行、640 列；CV_8UC1 表示每个像素是 1 个 8 位无符号整数（0～255）；初始值全是 0（黑）
    cv::Mat image(480, 640, CV_8UC1, cv::Scalar(0));
    std::cout << "行数（高）" << image.rows << "，列数（宽）" << image.cols << "，通道数 " << image.channels()
              << "，共 " << image.total() << " 个像素\n";
    c.check(image.rows == 480 && image.cols == 640, "先写行数（高），再写列数（宽）");
    c.check(image.type() == CV_8UC1, "类型是 CV_8UC1：8 位、无符号、1 个通道");

    // ---------- 2. 读写一个像素 ----------
    // at<类型>(行, 列)：先行后列，也就是先 y 后 x
    image.at<uchar>(100, 200) = 255;
    c.check(image.at<uchar>(100, 200) == 255, "at<uchar>(y, x) 读写一个像素");

    // 而 cv::Point 是 (x, y)。两种顺序正好相反，这是最常见的错误来源。
    const cv::Point p(200, 100);
    c.check(image.at<uchar>(p) == 255, "cv::Point(x, y) 和 at(y, x) 指的是同一个像素");

    // ---------- 3. 遍历所有像素：画一个从左到右由黑变白的渐变 ----------
    for (int y = 0; y < image.rows; ++y) {
        uchar* row = image.ptr<uchar>(y);  // 这一行第一个像素的地址
        for (int x = 0; x < image.cols; ++x) {
            row[x] = static_cast<uchar>(x * 255 / (image.cols - 1));
        }
    }
    c.check(image.at<uchar>(0, 0) == 0 && image.at<uchar>(0, 639) == 255, "渐变：最左边是 0，最右边是 255");
    cvtut::save(out / "01_gradient.png", image);

    // ---------- 4. 复制 Mat 不会复制像素 ----------
    cv::Mat same = image;           // 只复制了“信息头”，像素是共享的
    cv::Mat copy = image.clone();   // 真正复制了一份像素
    same.at<uchar>(10, 10) = 77;
    c.check(image.at<uchar>(10, 10) == 77, "cv::Mat b = a：改 b 的像素，a 也变了");
    copy.at<uchar>(20, 20) = 99;
    c.check(image.at<uchar>(20, 20) != 99, "a.clone()：改副本，a 不变");
    c.check(same.data == image.data && copy.data != image.data, "data 指针：共享的相同，clone 的不同");

    // ---------- 5. 取出一块区域（ROI）：同样不复制像素 ----------
    const cv::Rect area(200, 150, 240, 180);  // x, y, 宽, 高
    cv::Mat roi = image(area);
    c.check(roi.rows == 180 && roi.cols == 240, "ROI 的大小是 240×180");
    roi.setTo(0);                             // 把这块区域涂黑
    c.check(image.at<uchar>(200, 300) == 0, "往 ROI 里写，原图对应的位置也变了");
    cvtut::save(out / "01_roi_blackened.png", image);

    // ROI 的像素在内存里不是连续的：每一行后面隔着原图里不属于 ROI 的部分
    c.check(image.isContinuous() && !roi.isContinuous(), "整张图是连续的，ROI 不是");

    // ---------- 6. 用 OpenCV 的函数画图 ----------
    cv::Mat canvas(480, 640, CV_8UC1, cv::Scalar(170));
    cv::rectangle(canvas, cv::Rect(140, 110, 360, 260), cv::Scalar(70), cv::FILLED);
    cv::circle(canvas, cv::Point(320, 240), 60, cv::Scalar(230), 3);
    cv::putText(canvas, "OK", cv::Point(290, 255), cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(255), 2);
    cvtut::save(out / "01_drawing.png", canvas);

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
