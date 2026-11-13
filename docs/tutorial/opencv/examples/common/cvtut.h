#pragma once

// OpenCV 教程示例共用的小工具：输出目录、保存图像、生成示例图像、检查结果。
// 示例不弹出窗口显示图像（那需要额外的界面库），而是把结果保存成 PNG 文件，运行后打开来看。

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace cvtut {

namespace fs = std::filesystem;

// 输出目录：命令行的第一个参数；没给就用当前目录下的 out。目录不存在时会创建。
inline fs::path outputDir(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::path("out");
    fs::create_directories(dir);
    return dir;
}

// 保存图像。先在内存里编码，再自己写文件：这样路径里有中文也没问题（第 1 课第 6 节）。
inline bool save(const fs::path& file, const cv::Mat& image) {
    std::vector<uchar> bytes;
    if (!cv::imencode(file.extension().string(), image, bytes)) return false;
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// 读取图像。读不到时返回空的 Mat（empty() 为 true）。
inline cv::Mat load(const fs::path& file, int flags = cv::IMREAD_UNCHANGED) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    const std::vector<uchar> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return {};
    return cv::imdecode(bytes, flags);
}

// 给图像加一点噪声，让它更像相机拍出来的。种子固定，所以每次运行结果相同。
inline void addNoise(cv::Mat& image, double sigma, int seed = 42) {
    cv::RNG rng(static_cast<uint64>(seed));
    cv::Mat noise(image.size(), CV_16SC(image.channels()));
    rng.fill(noise, cv::RNG::NORMAL, cv::Scalar::all(0), cv::Scalar::all(sigma));
    cv::Mat wide;
    image.convertTo(wide, CV_16S);
    wide += noise;
    wide.convertTo(image, image.type());  // 转回 8 位时自动截断到 0～255
}

// 示例图像一：一个金属零件（灰度）。深色的零件、两个圆孔、一道亮的划痕。640×480。
inline cv::Mat makePartImage() {
    cv::Mat image(480, 640, CV_8UC1, cv::Scalar(170));
    cv::rectangle(image, cv::Rect(140, 110, 360, 260), cv::Scalar(70), cv::FILLED);
    cv::circle(image, cv::Point(220, 240), 35, cv::Scalar(170), cv::FILLED);
    cv::circle(image, cv::Point(420, 240), 35, cv::Scalar(170), cv::FILLED);
    cv::line(image, cv::Point(290, 150), cv::Point(350, 180), cv::Scalar(230), 2);
    addNoise(image, 3.0);
    return image;
}

// 示例图像二：一块电路板（彩色）。绿色的板子上有红、蓝、黄三种颜色的元件。640×480。
// 注意颜色的顺序是 蓝、绿、红（第 2 课）。
inline cv::Mat makeBoardImage() {
    cv::Mat image(480, 640, CV_8UC3, cv::Scalar(60, 60, 60));
    cv::rectangle(image, cv::Rect(60, 50, 520, 380), cv::Scalar(40, 120, 30), cv::FILLED);  // 绿色的板子
    cv::rectangle(image, cv::Rect(110, 100, 90, 50), cv::Scalar(30, 30, 200), cv::FILLED);   // 红色元件
    cv::rectangle(image, cv::Rect(400, 300, 90, 50), cv::Scalar(30, 30, 200), cv::FILLED);   // 红色元件
    cv::rectangle(image, cv::Rect(270, 180, 110, 110), cv::Scalar(200, 60, 30), cv::FILLED); // 蓝色芯片
    cv::circle(image, cv::Point(150, 330), 30, cv::Scalar(30, 210, 230), cv::FILLED);        // 黄色电容
    cv::circle(image, cv::Point(470, 130), 30, cv::Scalar(30, 210, 230), cv::FILLED);        // 黄色电容
    addNoise(image, 3.0);
    return image;
}

// 示例图像三：一块印着 24 个深色圆点的浅色标定板（灰度）。6 列 4 行，圆点半径 8。640×480。
inline cv::Mat makeDotsImage() {
    cv::Mat image(480, 640, CV_8UC1, cv::Scalar(200));
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 6; ++col) {
            cv::circle(image, cv::Point(95 + col * 90, 105 + row * 90), 8, cv::Scalar(60), cv::FILLED);
        }
    }
    addNoise(image, 3.0);
    return image;
}

// 模拟不均匀的光照：从左到右，亮度从 leftGain 倍均匀地变到 rightGain 倍。
inline cv::Mat applyShading(const cv::Mat& image, double leftGain, double rightGain) {
    cv::Mat result = image.clone();
    for (int y = 0; y < result.rows; ++y) {
        uchar* row = result.ptr<uchar>(y);
        for (int x = 0; x < result.cols; ++x) {
            const double gain = leftGain + (rightGain - leftGain) * x / (result.cols - 1);
            row[x] = cv::saturate_cast<uchar>(row[x] * gain);
        }
    }
    return result;
}

// 把灰度直方图画成一张图：横轴是灰度 0～255，纵轴是像素个数（按最高的一柱缩放）。
// markAt >= 0 时在那个灰度的位置画一条竖线（用来标出阈值）。
inline cv::Mat drawHistogram(const cv::Mat& histogram, int markAt = -1) {
    double maxCount = 0;
    cv::minMaxLoc(histogram, nullptr, &maxCount);
    cv::Mat canvas(220, 256 * 2, CV_8UC1, cv::Scalar(255));
    for (int g = 0; g < 256; ++g) {
        const int height = maxCount > 0 ? static_cast<int>(histogram.at<float>(g) / maxCount * 200) : 0;
        cv::rectangle(canvas, cv::Rect(g * 2, 210 - height, 2, height), cv::Scalar(60), cv::FILLED);
    }
    cv::line(canvas, cv::Point(0, 210), cv::Point(511, 210), cv::Scalar(0));
    if (markAt >= 0) cv::line(canvas, cv::Point(markAt * 2, 0), cv::Point(markAt * 2, 219), cv::Scalar(0), 1);
    return canvas;
}

// 检查结果是否符合预期。示例同时也是测试：有一项不符合，程序就以失败退出。
class Checker {
public:
    void check(bool ok, const std::string& what) {
        std::cout << (ok ? "  ✓ " : "  ✗ ") << what << "\n";
        if (!ok) ++failures_;
    }
    int exitCode() const {
        std::cout << (failures_ == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
        return failures_ == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

private:
    int failures_ = 0;
};

}  // namespace cvtut
