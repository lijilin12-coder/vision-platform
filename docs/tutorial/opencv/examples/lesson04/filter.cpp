// 第 4 课：滤波。用周围的像素算出新的像素值：去噪声，以及它的代价。
#include <cmath>

#include "cvtut.h"

// 一小块区域的标准差：用来衡量噪声有多大
double noiseLevel(const cv::Mat& image, const cv::Rect& area) {
    cv::Scalar mean, stddev;
    cv::meanStdDev(image(area), mean, stddev);
    return stddev[0];
}

// 零件左边缘（x = 140 附近）有多模糊：把第 150～330 行逐列取平均（消掉噪声），
// 数一数平均灰度介于“明显是背景”和“明显是零件”之间的列有几个。边缘越模糊，这个数越大。
int edgeWidth(const cv::Mat& image) {
    cv::Mat profile;
    cv::reduce(image(cv::Rect(100, 150, 80, 180)), profile, 0, cv::REDUCE_AVG, CV_32F);   // 每一列的平均值
    int count = 0;
    for (int x = 0; x < profile.cols; ++x) {
        const float v = profile.at<float>(0, x);
        if (v > 80 && v < 160) ++count;   // 背景 170，零件 70
    }
    return count;
}

// 椒盐噪声：随机把一些像素变成纯黑或纯白（模拟坏点、灰尘）
void addSaltAndPepper(cv::Mat& image, int count) {
    cv::RNG rng(7);
    for (int i = 0; i < count; ++i) {
        const int x = rng.uniform(0, image.cols), y = rng.uniform(0, image.rows);
        image.at<uchar>(y, x) = (i % 2 == 0) ? 255 : 0;
    }
}

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;
    const cv::Rect background(10, 10, 100, 80);   // 背景上的一块，用来量噪声

    // ---------- 1. 一张噪声比较大的图像 ----------
    cv::Mat noisy(480, 640, CV_8UC1, cv::Scalar(170));
    cv::rectangle(noisy, cv::Rect(140, 110, 360, 260), cv::Scalar(70), cv::FILLED);
    cvtut::addNoise(noisy, 20.0);
    cvtut::save(out / "04_noisy.png", noisy);
    const double before = noiseLevel(noisy, background);
    std::cout << "原图：噪声（背景的标准差）" << before << "，边缘宽度 " << edgeWidth(noisy) << " 个像素\n";

    // 直接做阈值：背景上会冒出很多噪声点
    cv::Mat noisyMask;
    cv::threshold(noisy, noisyMask, 120, 255, cv::THRESH_BINARY_INV);
    cvtut::save(out / "04_noisy_threshold.png", noisyMask);
    const int wrongBefore = cv::countNonZero(noisyMask(background));
    std::cout << "直接阈值分割：背景那一小块（8000 个像素）里有 " << wrongBefore << " 个被错分成零件\n";
    c.check(wrongBefore > 20, "噪声大时，直接阈值分割会有很多错分的点");

    // ---------- 2. 均值滤波：每个像素换成它周围 5×5 个像素的平均值 ----------
    cv::Mat mean5;
    cv::blur(noisy, mean5, cv::Size(5, 5));
    cvtut::save(out / "04_mean_5.png", mean5);
    const double afterMean = noiseLevel(mean5, background);
    std::cout << "5×5 均值滤波：噪声 " << afterMean << "（降到原来的 1/" << before / afterMean << "），边缘宽度 "
              << edgeWidth(mean5) << "\n";
    c.check(std::abs(before / afterMean - 5.0) < 0.7, "25 个像素取平均，噪声降到约 1/5（根号 25）");
    c.check(edgeWidth(mean5) >= 3, "代价：边缘变模糊了");

    cv::Mat meanMask;
    cv::threshold(mean5, meanMask, 120, 255, cv::THRESH_BINARY_INV);
    cvtut::save(out / "04_mean_5_threshold.png", meanMask);
    c.check(cv::countNonZero(meanMask(background)) == 0, "滤波之后再做阈值：背景上没有错分的点了");

    // 窗口越大，噪声越小，边缘越模糊
    cv::Mat mean15;
    cv::blur(noisy, mean15, cv::Size(15, 15));
    cvtut::save(out / "04_mean_15.png", mean15);
    std::cout << "15×15 均值滤波：噪声 " << noiseLevel(mean15, background) << "，边缘宽度 " << edgeWidth(mean15) << "\n";
    c.check(edgeWidth(mean15) > edgeWidth(mean5) * 2, "窗口从 5 加大到 15：边缘更模糊");

    // ---------- 3. 自己写一遍 3×3 均值滤波 ----------
    cv::Mat mine = noisy.clone();
    for (int y = 1; y < noisy.rows - 1; ++y) {
        for (int x = 1; x < noisy.cols - 1; ++x) {
            int sum = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) sum += noisy.at<uchar>(y + dy, x + dx);   // 读的是原图
            mine.at<uchar>(y, x) = static_cast<uchar>((sum + 4) / 9);                      // 写到另一张图；+4 是四舍五入
        }
    }
    cv::Mat mean3;
    cv::blur(noisy, mean3, cv::Size(3, 3));
    const cv::Rect inner(1, 1, noisy.cols - 2, noisy.rows - 2);   // 最外面一圈的处理方式不同，不比较
    c.check(cv::norm(mine(inner), mean3(inner), cv::NORM_INF) == 0, "手写的 3×3 均值滤波和 cv::blur 完全相同");

    // ---------- 4. 高斯滤波：离中心越近的像素权重越大 ----------
    cv::Mat gauss;
    cv::GaussianBlur(noisy, gauss, cv::Size(5, 5), 0);
    cvtut::save(out / "04_gaussian_5.png", gauss);
    std::cout << "5×5 高斯滤波：噪声 " << noiseLevel(gauss, background) << "，边缘宽度 " << edgeWidth(gauss) << "\n";
    c.check(noiseLevel(gauss, background) < before / 2, "高斯滤波同样能降低噪声");

    // ---------- 5. 椒盐噪声：均值滤波对付不了，中值滤波可以 ----------
    cv::Mat clean(480, 640, CV_8UC1, cv::Scalar(170));
    cv::rectangle(clean, cv::Rect(140, 110, 360, 260), cv::Scalar(70), cv::FILLED);
    cv::Mat peppered = clean.clone();
    addSaltAndPepper(peppered, 3000);
    cvtut::save(out / "04_salt_pepper.png", peppered);

    cv::Mat pepperedMean, pepperedMedian;
    cv::blur(peppered, pepperedMean, cv::Size(3, 3));
    cv::medianBlur(peppered, pepperedMedian, 3);          // 每个像素换成周围 3×3 个像素的中位数
    cvtut::save(out / "04_salt_pepper_mean.png", pepperedMean);
    cvtut::save(out / "04_salt_pepper_median.png", pepperedMedian);

    // 和没有噪声的原图比：有多少像素相差超过 5
    const auto wrongPixels = [&](const cv::Mat& image) {
        cv::Mat diff;
        cv::absdiff(image, clean, diff);
        return cv::countNonZero(diff(cv::Rect(0, 0, 130, 480)) > 5);   // 只看左边一条背景，避开零件的边缘
    };
    const int pepperedWrong = wrongPixels(peppered), meanWrong = wrongPixels(pepperedMean), medianWrong = wrongPixels(pepperedMedian);
    std::cout << "椒盐噪声：原来有 " << pepperedWrong << " 个坏点；均值滤波后 " << meanWrong << " 个像素不对；中值滤波后 "
              << medianWrong << " 个\n";
    c.check(meanWrong > pepperedWrong * 7, "均值滤波：每个坏点被抹开，弄脏了周围 3×3 个像素");
    c.check(medianWrong <= pepperedWrong / 50, "中值滤波：坏点几乎全部消失");

    // 中值滤波不会让边缘变模糊
    c.check(edgeWidth(pepperedMedian) == 0, "中值滤波之后，边缘仍然是“一刀切”的");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
