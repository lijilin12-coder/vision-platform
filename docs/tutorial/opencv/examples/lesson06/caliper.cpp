// 第 6 课：边缘（二）。卡尺：沿一条线找边缘的位置，精确到小于一个像素，用来测量尺寸。
#include <cmath>

#include "cvtut.h"

// 画一根竖直的深色长条，左右边缘在【不是整数】的位置上。
// 办法：先画一张横向放大 8 倍的图，再缩小回来。缩小时每个像素取 8 个小格的平均，
// 所以边缘所在的那个像素是“一部分背景、一部分长条”的中间灰度，和相机拍到的情况一样。
// left、right 是长条左右边缘的位置（单位：像素，可以是 1/8 的整数倍）。
cv::Mat makeBar(double left, double right, double sigmaNoise) {
    const int scale = 8;
    cv::Mat big(240, 640 * scale, CV_8UC1, cv::Scalar(180));
    cv::rectangle(big, cv::Rect(cvRound(left * scale), 0, cvRound((right - left) * scale), 240), cv::Scalar(60), cv::FILLED);
    cv::Mat image;
    cv::resize(big, image, cv::Size(640, 240), 0, 0, cv::INTER_AREA);
    cv::GaussianBlur(image, image, cv::Size(5, 5), 1.0);   // 镜头造成的一点模糊
    if (sigmaNoise > 0) cvtut::addNoise(image, sigmaNoise);
    return image;
}

// 取出一条水平的“亮度曲线”：第 y 行上下共 rows 行，逐列取平均。多行平均可以减小噪声。
std::vector<double> profileAlongRow(const cv::Mat& image, int y, int rows) {
    cv::Mat averaged;
    cv::reduce(image(cv::Rect(0, y - rows / 2, image.cols, rows)), averaged, 0, cv::REDUCE_AVG, CV_64F);
    return std::vector<double>(averaged.begin<double>(), averaged.end<double>());
}

enum class Polarity { BrightToDark, DarkToBright };

// 在 [from, to) 范围内找一条指定极性的边缘，返回它的位置（带小数）。找不到返回 -1。
double findEdge(const std::vector<double>& profile, int from, int to, Polarity polarity, double minStrength) {
    // 每个位置的梯度：右边的像素减左边的像素，除以 2
    const auto gradient = [&](int x) {
        const double g = (profile[static_cast<std::size_t>(x + 1)] - profile[static_cast<std::size_t>(x - 1)]) / 2;
        return polarity == Polarity::DarkToBright ? g : -g;   // 统一成“要找的那种边缘，梯度是正的”
    };
    int best = -1;
    double bestValue = minStrength;
    for (int x = std::max(from, 1); x < std::min(to, static_cast<int>(profile.size()) - 1); ++x) {
        if (gradient(x) > bestValue) bestValue = gradient(x), best = x;
    }
    if (best < 0) return -1;

    // 亚像素：梯度最大的点和它左右两个点，三个点确定一条抛物线，取抛物线的顶点
    const double a = gradient(best - 1), b = gradient(best), d = gradient(best + 1);
    const double denominator = a - 2 * b + d;
    const double offset = denominator != 0 ? 0.5 * (a - d) / denominator : 0.0;
    return best + offset;
}

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // 长条的左边缘在 200.375，右边缘在 400.625：真实宽度 200.25 个像素
    const double trueLeft = 200.375, trueRight = 400.625, trueWidth = trueRight - trueLeft;
    const cv::Mat bar = makeBar(trueLeft, trueRight, 3.0);
    cvtut::save(out / "06_bar.png", bar);

    // ---------- 1. 用阈值量宽度：结果取决于阈值 ----------
    std::cout << "真实宽度 " << trueWidth << " 个像素\n用阈值加数像素的办法量宽度（第 120 行）：\n";
    double minByThreshold = 1e9, maxByThreshold = 0;
    for (const int t : {90, 120, 150}) {
        cv::Mat mask;
        cv::threshold(bar.row(120), mask, t, 255, cv::THRESH_BINARY_INV);
        const int width = cv::countNonZero(mask);
        std::cout << "  阈值 " << t << "：" << width << " 个像素\n";
        minByThreshold = std::min<double>(minByThreshold, width);
        maxByThreshold = std::max<double>(maxByThreshold, width);
    }
    c.check(maxByThreshold - minByThreshold >= 1, "阈值不同，量出来的宽度相差 1 个像素以上");

    // ---------- 2. 卡尺：找梯度最大的位置 ----------
    const std::vector<double> profile = profileAlongRow(bar, 120, 21);   // 第 120 行上下共 21 行取平均
    const double left = findEdge(profile, 150, 250, Polarity::BrightToDark, 10);
    const double right = findEdge(profile, 350, 450, Polarity::DarkToBright, 10);
    const double width = right - left;
    std::cout << "卡尺：左边缘 " << left << "，右边缘 " << right << "，宽度 " << width << "（误差 " << width - trueWidth << "）\n";
    c.check(left > 0 && right > 0, "两条边缘都找到了");
    c.check(std::abs(width - trueWidth) < 0.1, "宽度误差小于 0.1 个像素");

    // 只取整数位置（不做抛物线插值）的话，误差是多少
    const double widthInteger = std::round(right) - std::round(left);
    std::cout << "如果只取整数位置：宽度 " << widthInteger << "（误差 " << widthInteger - trueWidth << "）\n";
    c.check(std::abs(widthInteger - trueWidth) > std::abs(width - trueWidth), "抛物线插值比只取整数位置更准");

    // ---------- 3. 光线变暗：结果不变 ----------
    const cv::Mat darker = bar * 0.6;
    const std::vector<double> darkProfile = profileAlongRow(darker, 120, 21);
    const double darkWidth = findEdge(darkProfile, 350, 450, Polarity::DarkToBright, 5) -
                             findEdge(darkProfile, 150, 250, Polarity::BrightToDark, 5);
    cv::Mat darkMask;
    cv::threshold(darker.row(120), darkMask, 120, 255, cv::THRESH_BINARY_INV);
    std::cout << "光线变暗到 60%：卡尺量出 " << darkWidth << "；固定阈值 120 量出 " << cv::countNonZero(darkMask) << "\n";
    c.check(std::abs(darkWidth - trueWidth) < 0.1, "卡尺：光线变暗之后，宽度误差仍小于 0.1 个像素");
    c.check(cv::countNonZero(darkMask) == darker.cols, "固定阈值：整行都被当成了长条");

    // ---------- 4. 多行平均的作用 ----------
    const cv::Mat noisyBar = makeBar(trueLeft, trueRight, 12.0);
    double worstSingle = 0, worstAveraged = 0;
    for (int y = 30; y <= 210; y += 20) {
        const auto single = profileAlongRow(noisyBar, y, 1);
        const auto averaged = profileAlongRow(noisyBar, y, 21);
        const double w1 = findEdge(single, 350, 450, Polarity::DarkToBright, 5) - findEdge(single, 150, 250, Polarity::BrightToDark, 5);
        const double w21 = findEdge(averaged, 350, 450, Polarity::DarkToBright, 5) - findEdge(averaged, 150, 250, Polarity::BrightToDark, 5);
        worstSingle = std::max(worstSingle, std::abs(w1 - trueWidth));
        worstAveraged = std::max(worstAveraged, std::abs(w21 - trueWidth));
    }
    std::cout << "噪声较大时，在 10 个位置各量一次：只用 1 行，最大误差 " << worstSingle << "；21 行取平均，最大误差 " << worstAveraged << "\n";
    c.check(worstAveraged < worstSingle && worstAveraged < 0.3, "多行取平均之后误差明显变小");

    // ---------- 5. 极性：同一段范围里，只找指定方向的边缘 ----------
    const double wrongPolarity = findEdge(profile, 150, 250, Polarity::DarkToBright, 10);
    c.check(wrongPolarity < 0, "在左边缘附近找“暗→亮”的边缘：找不到（那里只有“亮→暗”的边缘）");
    const double nothing = findEdge(profile, 20, 120, Polarity::BrightToDark, 10);
    c.check(nothing < 0, "在没有边缘的地方找：返回“没找到”，而不是硬给一个位置");

    // 把找到的边缘画出来
    cv::Mat drawn;
    cv::cvtColor(bar, drawn, cv::COLOR_GRAY2BGR);
    cv::rectangle(drawn, cv::Rect(150, 110, 300, 21), cv::Scalar(0, 200, 0), 1);                       // 卡尺的范围
    cv::line(drawn, cv::Point(cvRound(left), 95), cv::Point(cvRound(left), 145), cv::Scalar(0, 0, 255), 1);
    cv::line(drawn, cv::Point(cvRound(right), 95), cv::Point(cvRound(right), 145), cv::Scalar(0, 0, 255), 1);
    cvtut::save(out / "06_bar_measured.png", drawn);

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
