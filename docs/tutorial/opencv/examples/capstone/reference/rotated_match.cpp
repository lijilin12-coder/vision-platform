// 大作业的参考实现。建议先自己做完再看。
//
// 思路（对应任务书的四个阶段）：
//   1. 把模板转到某个角度，带着掩膜做一次模板匹配：得到“这个角度下最像的位置和得分”。
//   2. 由粗到细：在金字塔的最高层用大的角度步长试遍整个角度范围，留下几个候选；
//      然后逐层往下，每一层只在候选的位置和角度附近找，同时把角度步长减半。
//   3. 在原图上，用最高分和它两边的得分做抛物线插值，把角度和位置定到小数位。
//   4. 得分不够就返回“没找到”。
//
// 角度的约定：对外（RotatedMatch::angle）是“顺时针为正”；cv::getRotationMatrix2D 是“逆时针为正”。
// 两者只在 rotateTemplate 这一个函数里转换。
#include "rotated_match.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace capstone {
namespace {

const int kMaxLevels = 3;          // 金字塔最多用到第几层
const int kMinTemplateSide = 12;   // 最高层上模板的短边不能小于这么多像素（再小，细节就没了）
const int kCandidates = 5;         // 粗找之后保留几个候选
const double kFinestStep = 0.5;    // 原图上角度的最小步长（度）。更细的靠插值
const double kKeepWithin = 0.1;    // 细找时，得分比第一名低这么多以上的候选直接丢掉
const double kAngleBaseline = 1.5; // 角度插值时，取最高分两侧各多少度的得分

// 旋转后的模板：图像、掩膜（255 表示这个像素是模板的内容，0 表示是旋转后填充的）
struct RotatedTemplate {
    cv::Mat image;
    cv::Mat mask;
};

// 把模板顺时针转 angle 度。画布放大到能装下整个模板，模板中心落在新画布的中心。
RotatedTemplate rotateTemplate(const cv::Mat& templ, double angle) {
    const double radians = angle * CV_PI / 180.0;
    const double c = std::abs(std::cos(radians)), s = std::abs(std::sin(radians));
    const int width = static_cast<int>(std::ceil(templ.cols * c + templ.rows * s));
    const int height = static_cast<int>(std::ceil(templ.cols * s + templ.rows * c));

    const cv::Point2f center(templ.cols / 2.0f, templ.rows / 2.0f);
    cv::Mat m = cv::getRotationMatrix2D(center, -angle, 1.0);   // 逆时针为正，所以取负
    m.at<double>(0, 2) += width / 2.0 - center.x;
    m.at<double>(1, 2) += height / 2.0 - center.y;

    RotatedTemplate result;
    cv::warpAffine(templ, result.image, m, cv::Size(width, height), cv::INTER_LINEAR);
    const cv::Mat white(templ.size(), CV_8UC1, cv::Scalar(255));
    cv::warpAffine(white, result.mask, m, cv::Size(width, height), cv::INTER_NEAREST);
    // 掩膜向里缩一圈：最边上的像素是模板内容和填充值插值出来的，不可靠
    cv::erode(result.mask, result.mask, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
    return result;
}

// 带掩膜的归一化相关系数。模板盖住的地方如果完全均匀，得分的分母是 0，结果会是无穷大或“不是一个数”，
// 把这些位置的得分改成 -1（最不像）。
cv::Mat scoreMap(const cv::Mat& scene, const RotatedTemplate& t) {
    cv::Mat result;
    cv::matchTemplate(scene, t.image, result, cv::TM_CCOEFF_NORMED, t.mask);
    cv::patchNaNs(result, -1.0);
    result.setTo(-1.0, result > 1.0001);
    result.setTo(-1.0, result < -1.0001);
    return result;
}

struct Candidate {
    double x = 0, y = 0, angle = 0, score = -2;
};

// 在 scene 的一个小窗口里（中心 (x, y)，四周余量 margin），用转了 angle 度的模板找最高分。
// scoresAround 不为空时，同时返回最高分位置及其上下左右的得分（用于插值）。
Candidate matchNear(const cv::Mat& scene, const cv::Mat& templ, double x, double y, double angle, int margin,
                    double* scoresAround = nullptr) {
    const RotatedTemplate t = rotateTemplate(templ, angle);
    const cv::Rect wanted(cvRound(x - t.image.cols / 2.0) - margin, cvRound(y - t.image.rows / 2.0) - margin,
                          t.image.cols + 2 * margin, t.image.rows + 2 * margin);
    const cv::Rect window = wanted & cv::Rect(0, 0, scene.cols, scene.rows);
    if (window.width < t.image.cols || window.height < t.image.rows) return {};   // 模板伸到图像外面了

    const cv::Mat scores = scoreMap(scene(window), t);
    double best = 0;
    cv::Point where;
    cv::minMaxLoc(scores, nullptr, &best, nullptr, &where);

    if (scoresAround != nullptr) {
        const auto at = [&](int dx, int dy) {
            const int px = where.x + dx, py = where.y + dy;
            return (px >= 0 && py >= 0 && px < scores.cols && py < scores.rows) ? static_cast<double>(scores.at<float>(py, px)) : best;
        };
        scoresAround[0] = at(-1, 0);
        scoresAround[1] = at(1, 0);
        scoresAround[2] = at(0, -1);
        scoresAround[3] = at(0, 1);
    }
    // 得分图里的坐标是模板左上角；加上窗口的偏移和模板的一半，换成模板中心在整张图里的位置
    return {window.x + where.x + t.image.cols / 2.0, window.y + where.y + t.image.rows / 2.0, angle, best};
}

// 三个等间隔的点 (−1, a)、(0, b)、(1, c)，b 最大：返回抛物线顶点的位置，在 −0.5～0.5 之间
double parabolaPeak(double a, double b, double c) {
    const double denominator = a - 2 * b + c;
    if (denominator >= 0) return 0;   // 不是一个“峰”，不插值
    return std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
}

double normalizeAngle(double angle) {
    angle = std::fmod(angle, 360.0);
    return angle < 0 ? angle + 360.0 : angle;
}

}  // namespace

std::optional<RotatedMatch> findRotated(const cv::Mat& scene, const cv::Mat& templ, const MatchOptions& options) {
    if (scene.empty() || templ.empty() || scene.type() != CV_8UC1 || templ.type() != CV_8UC1) return std::nullopt;
    if (templ.cols > scene.cols || templ.rows > scene.rows) return std::nullopt;

    // ---- 建金字塔。层数由模板的大小决定：最高层上模板不能太小 ----
    int levels = 0;
    while (levels < kMaxLevels && (std::min(templ.cols, templ.rows) >> (levels + 1)) >= kMinTemplateSide) ++levels;
    std::vector<cv::Mat> scenes = {scene}, templates = {templ};
    for (int i = 0; i < levels; ++i) {
        cv::Mat s, t;
        cv::pyrDown(scenes.back(), s);
        cv::pyrDown(templates.back(), t);
        scenes.push_back(s);
        templates.push_back(t);
    }

    // ---- 粗找：最高层，整个角度范围 ----
    // 角度步长：让模板最远的角上的像素，每一步移动大约 1 个像素。模板越小，步长可以越大。
    const cv::Mat& topTemplate = templates[static_cast<std::size_t>(levels)];
    const cv::Mat& topScene = scenes[static_cast<std::size_t>(levels)];
    const double radius = 0.5 * std::hypot(topTemplate.cols, topTemplate.rows);
    const double coarseStep = std::clamp(std::atan(1.0 / radius) * 180.0 / CV_PI, 1.0, 10.0);
    const int steps = std::max(1, static_cast<int>(std::ceil(options.angleExtent / coarseStep)));

    std::vector<Candidate> candidates;
    for (int i = 0; i < steps; ++i) {
        const double angle = options.angleStart + i * coarseStep;
        const RotatedTemplate t = rotateTemplate(topTemplate, angle);
        if (t.image.cols > topScene.cols || t.image.rows > topScene.rows) continue;
        const cv::Mat scores = scoreMap(topScene, t);
        double best = 0;
        cv::Point where;
        cv::minMaxLoc(scores, nullptr, &best, nullptr, &where);
        candidates.push_back({where.x + t.image.cols / 2.0, where.y + t.image.rows / 2.0, angle, best});
    }
    if (candidates.empty()) return std::nullopt;

    // 只留得分最高的几个。不只留一个，是因为最高层上细节少，真正的目标不一定排第一。
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    if (candidates.size() > static_cast<std::size_t>(kCandidates)) candidates.resize(static_cast<std::size_t>(kCandidates));

    // ---- 细找：逐层往下。每一层只在候选的位置和角度附近找，角度步长减半 ----
    double step = coarseStep;
    for (int level = levels - 1; level >= 0; --level) {
        const cv::Mat& s = scenes[static_cast<std::size_t>(level)];
        const cv::Mat& t = templates[static_cast<std::size_t>(level)];
        step = std::max(step / 2, kFinestStep);
        std::vector<Candidate> refinedAll;
        for (const Candidate& candidate : candidates) {
            Candidate refined;
            for (const double angle : {candidate.angle - step, candidate.angle, candidate.angle + step}) {
                // 上一层的坐标乘以 2，就是这一层的坐标
                const Candidate c = matchNear(s, t, candidate.x * 2, candidate.y * 2, angle, 3);
                if (c.score > refined.score) refined = c;
            }
            if (refined.score > -1.5) refinedAll.push_back(refined);
        }
        if (refinedAll.empty()) return std::nullopt;
        // 越往下细节越多，真假越容易分清：明显不如第一名的候选不再往下带，省时间
        std::sort(refinedAll.begin(), refinedAll.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        candidates.clear();
        for (const Candidate& c : refinedAll)
            if (c.score >= refinedAll.front().score - kKeepWithin) candidates.push_back(c);
    }
    Candidate best = candidates.front();

    // 到了原图之后，继续把角度步长减半，直到最小步长
    while (step > kFinestStep) {
        step = std::max(step / 2, kFinestStep);
        for (const double angle : {best.angle - step, best.angle + step}) {
            const Candidate c = matchNear(scene, templ, best.x, best.y, angle, 2);
            if (c.score > best.score) best = c;
        }
    }
    const double bestStep = step;
    if (best.score < options.minScore) return std::nullopt;

    // ---- 确保停在得分的“山顶”上：只要旁边一步的角度得分更高，就挪过去 ----
    for (int i = 0; i < 8; ++i) {
        const Candidate left = matchNear(scene, templ, best.x, best.y, best.angle - bestStep, 2);
        const Candidate right = matchNear(scene, templ, best.x, best.y, best.angle + bestStep, 2);
        if (left.score <= best.score && right.score <= best.score) break;
        best = left.score > right.score ? left : right;
    }

    // ---- 插值：角度和位置都定到小数位 ----
    double around[4] = {0, 0, 0, 0};
    const Candidate center = matchNear(scene, templ, best.x, best.y, best.angle, 2, around);
    // 角度插值的间隔取得比最小步长宽：相邻两步的得分差太小，容易被噪声盖住
    const double delta = kAngleBaseline;
    const Candidate before = matchNear(scene, templ, best.x, best.y, best.angle - delta, 2);
    const Candidate after = matchNear(scene, templ, best.x, best.y, best.angle + delta, 2);

    RotatedMatch result;
    result.angle = normalizeAngle(center.angle + delta * parabolaPeak(before.score, center.score, after.score));
    result.x = center.x + parabolaPeak(around[0], center.score, around[1]);
    result.y = center.y + parabolaPeak(around[2], center.score, around[3]);
    result.score = center.score;
    return result;
}

}  // namespace capstone
