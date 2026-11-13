// 第 3 课：直方图。一张图像里各种亮度的像素各有多少。
#include "cvtut.h"

// 计算 8 位灰度图像的直方图：返回 256 行 1 列的 float 矩阵，第 g 行是灰度为 g 的像素个数
cv::Mat grayHistogram(const cv::Mat& gray) {
    cv::Mat histogram;
    const int channels[] = {0};
    const int size[] = {256};
    const float range[] = {0, 256};
    const float* ranges[] = {range};
    cv::calcHist(&gray, 1, channels, cv::Mat(), histogram, 1, size, ranges);
    return histogram;
}

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    const cv::Mat part = cvtut::makePartImage();
    cvtut::save(out / "03_part.png", part);

    // ---------- 1. 自己数一遍：直方图就是计数 ----------
    std::vector<int> counts(256, 0);
    for (int y = 0; y < part.rows; ++y) {
        const uchar* row = part.ptr<uchar>(y);
        for (int x = 0; x < part.cols; ++x) ++counts[row[x]];
    }

    // ---------- 2. 用 OpenCV 计算 ----------
    const cv::Mat histogram = grayHistogram(part);
    bool same = true;
    for (int g = 0; g < 256; ++g) same = same && counts[static_cast<std::size_t>(g)] == static_cast<int>(histogram.at<float>(g));
    c.check(same, "calcHist 的结果和自己数的完全相同");
    c.check(cv::sum(histogram)[0] == part.total(), "所有柱子加起来等于像素总数");
    cvtut::save(out / "03_histogram_part.png", cvtut::drawHistogram(histogram));

    // ---------- 3. 直方图有两个峰：零件和背景 ----------
    // 在 0～119 和 120～255 两段里分别找最高的柱子
    int darkPeak = 0, brightPeak = 120;
    for (int g = 0; g < 120; ++g) if (counts[static_cast<std::size_t>(g)] > counts[static_cast<std::size_t>(darkPeak)]) darkPeak = g;
    for (int g = 120; g < 256; ++g) if (counts[static_cast<std::size_t>(g)] > counts[static_cast<std::size_t>(brightPeak)]) brightPeak = g;
    std::cout << "暗的峰在灰度 " << darkPeak << "（零件），亮的峰在灰度 " << brightPeak << "（背景）\n";
    c.check(std::abs(darkPeak - 70) <= 2 && std::abs(brightPeak - 170) <= 2, "两个峰分别在 70 和 170 附近，就是画图时用的两个灰度");

    // 两个峰之间几乎没有像素
    int between = 0;
    for (int g = 90; g <= 150; ++g) between += counts[static_cast<std::size_t>(g)];
    std::cout << "灰度 90～150 之间的像素只有 " << between << " 个（共 " << part.total() << " 个）\n";
    c.check(between < 1500, "两个峰之间是一段很宽的“谷”");

    // ---------- 4. 峰的宽度来自噪声 ----------
    cv::Scalar mean, stddev;
    cv::meanStdDev(part(cv::Rect(10, 10, 100, 80)), mean, stddev);  // 背景上的一小块
    std::cout << "背景上一小块：平均灰度 " << mean[0] << "，标准差 " << stddev[0] << "\n";
    c.check(std::abs(mean[0] - 170) < 1 && std::abs(stddev[0] - 3) < 0.5, "平均值 170，标准差约 3（生成图像时加的噪声就是 3）");

    // ---------- 5. 光线变化时，直方图整体移动 ----------
    const cv::Mat darker = part * 0.6;                    // 光线变暗到 60%
    cvtut::save(out / "03_part_darker.png", darker);
    cvtut::save(out / "03_histogram_darker.png", cvtut::drawHistogram(grayHistogram(darker)));
    cv::meanStdDev(darker(cv::Rect(10, 10, 100, 80)), mean, stddev);
    std::cout << "变暗之后背景的平均灰度 " << mean[0] << "\n";
    c.check(std::abs(mean[0] - 102) < 1.5, "背景从 170 变成约 102：两个峰都向左移，并且靠得更近了");

    // ---------- 6. 光照不均匀时，峰被拉宽，甚至互相重叠 ----------
    const cv::Mat shaded = cvtut::applyShading(part, 1.0, 0.35);  // 左边正常，右边只有 35% 的亮度
    cvtut::save(out / "03_part_shaded.png", shaded);
    cvtut::save(out / "03_histogram_shaded.png", cvtut::drawHistogram(grayHistogram(shaded)));
    std::cout << "光照不均匀：左边的背景 " << int(shaded.at<uchar>(240, 20)) << "，左边的零件 " << int(shaded.at<uchar>(130, 160))
              << "，右边的背景 " << int(shaded.at<uchar>(240, 620)) << "\n";
    c.check(shaded.at<uchar>(240, 620) < shaded.at<uchar>(130, 160) + 10, "右边的背景和左边的零件差不多一样暗：直方图里它们混在一起");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
