// 大作业的评分程序。生成一批已知位置和角度的测试图像，调用 findRotated，统计误差和耗时。
//
//   用法：cv09_evaluate_student [输出目录]      评你的实现
//         cv09_evaluate_reference [输出目录]    评参考实现
//
// 两个程序用的是同一份评分代码，只是链接的 rotated_match.cpp 不同。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>

#include "cvtut.h"
#include "rotated_match.h"

namespace {

struct Stats {
    int total = 0;
    int found = 0;        // 返回了结果
    int correct = 0;      // 返回的结果在及格范围内
    double maxPosition = 0, sumPosition = 0;
    double maxAngle = 0, sumAngle = 0;
    double sumMs = 0, maxMs = 0;
};

// 及格线：位置误差不超过 1 个像素，角度误差不超过 0.5 度
const double kPositionLimit = 1.0;
const double kAngleLimit = 0.5;
// 优秀线：位置误差不超过 0.3 个像素，角度误差不超过 0.2 度（要做到这一步，需要插值）
const double kPositionExcellent = 0.3;
const double kAngleExcellent = 0.2;

double angleDifference(double a, double b) {
    double d = std::fmod(std::abs(a - b), 360.0);
    return d > 180 ? 360 - d : d;
}

// 在场景上画出找到的位置和角度：一个转过相应角度的矩形框，和一条指示方向的线
void drawPose(cv::Mat& image, const capstone::RotatedMatch& m, cv::Size templateSize, const cv::Scalar& color) {
    cv::Point2f corners[4];
    cv::RotatedRect({static_cast<float>(m.x), static_cast<float>(m.y)}, templateSize, static_cast<float>(m.angle)).points(corners);
    for (int i = 0; i < 4; ++i) cv::line(image, corners[i], corners[(i + 1) % 4], color, 2);
    const double radians = m.angle * CV_PI / 180;
    cv::line(image, cv::Point2d(m.x, m.y), cv::Point2d(m.x + 50 * std::cos(radians), m.y + 50 * std::sin(radians)), color, 2);
}

void print(const char* name, const Stats& s) {
    std::cout << std::fixed << std::setprecision(2) << "  " << name << "：" << s.correct << "/" << s.total << " 及格";
    if (s.correct > 0) {
        std::cout << "；位置误差 平均 " << s.sumPosition / s.correct << " 最大 " << s.maxPosition << " 像素；角度误差 平均 "
                  << s.sumAngle / s.correct << " 最大 " << s.maxAngle << " 度";
    }
    std::cout << "；耗时 平均 " << s.sumMs / s.total << " 最大 " << s.maxMs << " 毫秒\n";
}

}  // namespace

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);

    // 模板：标记件，四周留 4 个像素的背景
    const cv::Mat templ = cvtut::makeWidget()(cv::Rect(16, 16, 168, 108)).clone();
    cvtut::save(out / "09_template.png", templ);

    cv::RNG rng(2026);   // 种子固定：每次运行、每台机器上的测试图像都相同
    const auto randomPlacement = [&](double angleFrom, double angleTo) {
        return cvtut::Placement{rng.uniform(130.0, 510.0), rng.uniform(130.0, 250.0), rng.uniform(angleFrom, angleTo)};
    };
    const auto run = [&](const cv::Mat& scene, const cvtut::Placement& truth, Stats& stats, const std::string& saveAs = "") {
        const auto start = std::chrono::steady_clock::now();
        const auto match = capstone::findRotated(scene, templ);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ++stats.total;
        stats.sumMs += ms;
        stats.maxMs = std::max(stats.maxMs, ms);
        if (match) {
            ++stats.found;
            const double dp = std::hypot(match->x - truth.x, match->y - truth.y), da = angleDifference(match->angle, truth.angle);
            if (dp <= kPositionLimit && da <= kAngleLimit) {
                ++stats.correct;
                stats.sumPosition += dp;
                stats.maxPosition = std::max(stats.maxPosition, dp);
                stats.sumAngle += da;
                stats.maxAngle = std::max(stats.maxAngle, da);
            } else {
                std::cout << "    ✗ 真实 (" << truth.x << ", " << truth.y << ") " << truth.angle << " 度；找到 (" << match->x << ", " << match->y
                          << ") " << match->angle << " 度，得分 " << match->score << "\n";
            }
        } else {
            std::cout << "    ✗ 真实 (" << truth.x << ", " << truth.y << ") " << truth.angle << " 度；返回“没找到”\n";
        }
        if (!saveAs.empty()) {
            cv::Mat drawn;
            cv::cvtColor(scene, drawn, cv::COLOR_GRAY2BGR);
            if (match) drawPose(drawn, *match, templ.size(), cv::Scalar(0, 200, 0));
            cvtut::save(out / saveAs, drawn);
        }
    };

    std::cout << "及格线：位置误差 ≤ " << kPositionLimit << " 像素，角度误差 ≤ " << kAngleLimit << " 度\n";

    // ---- A 组：小角度（±10 度以内），没有干扰物 ----
    Stats a;
    for (int i = 0; i < 10; ++i) {
        const auto p = randomPlacement(-10, 10);
        run(cvtut::makeScene({p}, 3.0, false), p, a, i == 0 ? "09_a_small_angle.png" : "");
    }
    print("A 小角度（±10 度）  ", a);

    // ---- B 组：任意角度，有干扰物 ----
    Stats b;
    for (int i = 0; i < 30; ++i) {
        const auto p = randomPlacement(0, 360);
        run(cvtut::makeScene({p}, 3.0), p, b, i < 3 ? "09_b_any_angle_" + std::to_string(i) + ".png" : "");
    }
    print("B 任意角度          ", b);

    // ---- C 组：任意角度，噪声更大，光线变暗 ----
    Stats c;
    for (int i = 0; i < 20; ++i) {
        const auto p = randomPlacement(0, 360);
        cv::Mat scene = cvtut::makeScene({p}, 0.0);
        scene = scene * 0.6 + 15;          // 变暗，对比度降低
        cvtut::addNoise(scene, 8.0, 100 + i);
        run(scene, p, c, i == 0 ? "09_c_dark_noisy.png" : "");
    }
    print("C 变暗并且噪声更大  ", c);

    // ---- D 组：场景里没有目标，应该返回“没找到” ----
    int falseAlarms = 0;
    const int emptyScenes = 10;
    for (int i = 0; i < emptyScenes; ++i) {
        cv::Mat scene = cvtut::makeScene({}, 0.0);
        cvtut::addNoise(scene, 3.0 + i, 200 + i);
        const auto match = capstone::findRotated(scene, templ);
        if (match) {
            ++falseAlarms;
            std::cout << "    ✗ 场景里没有目标，却返回了 (" << match->x << ", " << match->y << ") " << match->angle << " 度，得分 " << match->score << "\n";
        }
    }
    std::cout << "  D 没有目标的场景    ：" << emptyScenes - falseAlarms << "/" << emptyScenes << " 正确地返回“没找到”\n";

    const bool passed = a.correct == a.total && b.correct == b.total && c.correct == c.total && falseAlarms == 0;
    const double worstPosition = std::max({a.maxPosition, b.maxPosition, c.maxPosition});
    const double worstAngle = std::max({a.maxAngle, b.maxAngle, c.maxAngle});
    const bool excellent = passed && worstPosition <= kPositionExcellent && worstAngle <= kAngleExcellent;
    std::cout << "\n" << (passed ? "及格：全部通过" : "及格：没有全部通过") << "\n";
    if (passed) {
        std::cout << "优秀（位置 ≤ " << kPositionExcellent << " 像素，角度 ≤ " << kAngleExcellent << " 度）："
                  << (excellent ? "达到" : "没有达到") << "。最大位置误差 " << worstPosition << "，最大角度误差 " << worstAngle << "\n";
    }
    std::cout << "结果图像保存在：" << out.string() << "\n";
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
