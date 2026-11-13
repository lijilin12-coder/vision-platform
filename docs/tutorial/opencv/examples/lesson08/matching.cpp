// 第 8 课：模板匹配。拿一小块图像（模板）在大图里到处比，找出最像的位置。
#include <chrono>
#include <cmath>

#include "cvtut.h"

// 模板匹配：返回最像的位置（模板中心在大图里的坐标）和得分
struct Match {
    double x = 0, y = 0, score = 0;
};

Match bestMatch(const cv::Mat& scene, const cv::Mat& templ, int method = cv::TM_CCOEFF_NORMED) {
    cv::Mat result;
    cv::matchTemplate(scene, templ, result, method);
    double minValue = 0, maxValue = 0;
    cv::Point minLocation, maxLocation;
    cv::minMaxLoc(result, &minValue, &maxValue, &minLocation, &maxLocation);
    // 平方差是“越小越像”，其他方法是“越大越像”
    const bool smallerIsBetter = method == cv::TM_SQDIFF || method == cv::TM_SQDIFF_NORMED;
    const cv::Point topLeft = smallerIsBetter ? minLocation : maxLocation;
    // 结果图里的坐标是模板【左上角】的位置；加上模板的一半，换成模板中心的位置
    return {topLeft.x + templ.cols / 2.0, topLeft.y + templ.rows / 2.0, smallerIsBetter ? minValue : maxValue};
}

// 找出所有得分不低于 minScore 的位置：每次取最高分，然后把它周围抹掉，再取下一个
std::vector<Match> allMatches(const cv::Mat& scene, const cv::Mat& templ, double minScore) {
    cv::Mat result;
    cv::matchTemplate(scene, templ, result, cv::TM_CCOEFF_NORMED);
    std::vector<Match> found;
    while (true) {
        double maxValue = 0;
        cv::Point maxLocation;
        cv::minMaxLoc(result, nullptr, &maxValue, nullptr, &maxLocation);
        if (maxValue < minScore) break;
        found.push_back({maxLocation.x + templ.cols / 2.0, maxLocation.y + templ.rows / 2.0, maxValue});
        // 把这个峰周围半个模板大小的范围抹掉：那里的高分都属于同一个目标
        const cv::Rect around(maxLocation.x - templ.cols / 2, maxLocation.y - templ.rows / 2, templ.cols, templ.rows);
        result(around & cv::Rect(0, 0, result.cols, result.rows)).setTo(-1);
    }
    return found;
}

int main(int argc, char** argv) {
    const auto out = cvtut::outputDir(argc, argv);
    cvtut::Checker c;

    // 模板：标记件本身，四周留 4 个像素的背景
    const cv::Mat widget = cvtut::makeWidget();                   // 200×140，零件在正中间
    const cv::Mat templ = widget(cv::Rect(16, 16, 168, 108)).clone();
    cvtut::save(out / "08_template.png", templ);

    // ---------- 1. 基本用法 ----------
    const cvtut::Placement truth{250, 150, 0};
    const cv::Mat scene = cvtut::makeScene({truth});
    cvtut::save(out / "08_scene.png", scene);

    cv::Mat result;
    cv::matchTemplate(scene, templ, result, cv::TM_CCOEFF_NORMED);
    std::cout << "场景 " << scene.cols << "×" << scene.rows << "，模板 " << templ.cols << "×" << templ.rows << "，结果图 " << result.cols << "×"
              << result.rows << "，类型是 32 位浮点数\n";
    c.check(result.cols == scene.cols - templ.cols + 1 && result.rows == scene.rows - templ.rows + 1, "结果图的大小 = 场景 − 模板 + 1");
    c.check(result.type() == CV_32FC1, "结果图每个像素是一个 float：模板放在这个位置时的得分");

    cv::Mat resultImage;
    result.convertTo(resultImage, CV_8U, 127.5, 127.5);   // 得分 -1～1 映射到 0～255，方便看
    cvtut::save(out / "08_score_map.png", resultImage);

    const Match m = bestMatch(scene, templ);
    std::cout << "最高分 " << m.score << "，位置 (" << m.x << ", " << m.y << ")；真实位置 (" << truth.x << ", " << truth.y << ")\n";
    c.check(std::hypot(m.x - truth.x, m.y - truth.y) < 1.0, "找到的位置和真实位置相差不到 1 个像素");
    c.check(m.score > 0.95, "得分接近 1");

    // ---------- 2. 三种算法 ----------
    const Match bySqdiff = bestMatch(scene, templ, cv::TM_SQDIFF);
    const Match byCcorr = bestMatch(scene, templ, cv::TM_CCORR);
    std::cout << "平方差 TM_SQDIFF：位置 (" << bySqdiff.x << ", " << bySqdiff.y << ")\n";
    std::cout << "相关 TM_CCORR（不归一化）：位置 (" << byCcorr.x << ", " << byCcorr.y << ")\n";
    c.check(std::hypot(bySqdiff.x - truth.x, bySqdiff.y - truth.y) < 1.0, "平方差：位置正确");
    c.check(std::hypot(byCcorr.x - truth.x, byCcorr.y - truth.y) > 100, "不归一化的相关：找到了那个很亮的矩形上（哪里亮，哪里得分高）");

    // 光线变化：整体变暗并且对比度降低
    const cv::Mat dim = scene * 0.6 + 20;
    const Match dimSqdiff = bestMatch(dim, templ, cv::TM_SQDIFF), dimCoeff = bestMatch(dim, templ, cv::TM_CCOEFF_NORMED);
    std::cout << "光线变化之后：平方差的最小值从 " << bySqdiff.score << " 变成 " << dimSqdiff.score << "；归一化相关系数从 " << m.score << " 变成 "
              << dimCoeff.score << "\n";
    c.check(dimSqdiff.score > bySqdiff.score * 10, "平方差的数值变了十倍以上：没法用一个固定的门限判断“像不像”");
    c.check(std::abs(dimCoeff.score - m.score) < 0.05 && std::hypot(dimCoeff.x - truth.x, dimCoeff.y - truth.y) < 1.0,
            "归一化相关系数：得分和位置都几乎不变");

    // ---------- 3. 得分用来判断“有没有” ----------
    const cv::Mat empty = cvtut::makeScene({});   // 没有标记件，只有干扰物
    cvtut::save(out / "08_scene_empty.png", empty);
    const Match none = bestMatch(empty, templ);
    std::cout << "场景里没有标记件：最高分 " << none.score << "，位置 (" << none.x << ", " << none.y << ")\n";
    c.check(none.score < 0.7, "最高分明显低于 1：模板匹配总会给出一个“最像的位置”，要靠得分判断它是不是真的");

    // ---------- 4. 找多个 ----------
    const std::vector<cvtut::Placement> three = {{130, 90, 0}, {420, 120, 0}, {300, 270, 0}};
    const cv::Mat sceneThree = cvtut::makeScene(three);
    cvtut::save(out / "08_scene_three.png", sceneThree);
    const auto found = allMatches(sceneThree, templ, 0.8);
    std::cout << "三个标记件的场景：找到 " << found.size() << " 个";
    for (const auto& f : found) std::cout << "  (" << f.x << ", " << f.y << ") 得分 " << f.score;
    std::cout << "\n";
    c.check(found.size() == 3, "找到 3 个");
    bool allCorrect = found.size() == 3;
    for (const auto& t : three) {
        bool hit = false;
        for (const auto& f : found) hit = hit || std::hypot(f.x - t.x, f.y - t.y) < 1.0;
        allCorrect = allCorrect && hit;
    }
    c.check(allCorrect, "三个位置都正确");

    cv::Mat drawn;
    cv::cvtColor(sceneThree, drawn, cv::COLOR_GRAY2BGR);
    for (const auto& f : found)
        cv::rectangle(drawn, cv::Rect(cvRound(f.x - templ.cols / 2.0), cvRound(f.y - templ.rows / 2.0), templ.cols, templ.rows), cv::Scalar(0, 200, 0), 2);
    cvtut::save(out / "08_scene_three_found.png", drawn);

    // ---------- 5. 模板匹配最大的弱点：目标转了一个角度 ----------
    std::cout << "目标转过不同角度时的最高分：\n";
    std::vector<double> scores;
    for (const double angle : {0.0, 1.0, 2.0, 5.0, 10.0, 20.0, 45.0, 90.0, 180.0}) {
        const cv::Mat rotatedScene = cvtut::makeScene({{250, 180, angle}}, 3.0, false);
        const Match r = bestMatch(rotatedScene, templ);
        scores.push_back(r.score);
        std::cout << "  " << angle << " 度：得分 " << r.score << "，位置误差 " << std::hypot(r.x - 250, r.y - 180) << " 个像素\n";
        if (angle == 20.0) cvtut::save(out / "08_scene_rotated_20.png", rotatedScene);
    }
    c.check(scores[0] > 0.95, "0 度：得分接近 1");
    c.check(scores[2] > 0.8, "2 度：还可以");
    c.check(scores[4] < 0.75, "10 度：得分已经掉到 0.75 以下");
    c.check(scores[6] < 0.5, "45 度：基本认不出来了");

    // ---------- 6. 用金字塔加速 ----------
    // 放大成一张大图，时间才量得准
    cv::Mat bigScene, bigTempl;
    cv::resize(scene, bigScene, cv::Size(), 3, 3, cv::INTER_LINEAR);     // 1920×1440
    cv::resize(templ, bigTempl, cv::Size(), 3, 3, cv::INTER_LINEAR);
    const auto now = [] { return std::chrono::steady_clock::now(); };
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    const auto t0 = now();
    const Match direct = bestMatch(bigScene, bigTempl);
    const auto t1 = now();

    // 由粗到细：在第 3 层（1/8）上找大致位置，再回到原图，只在周围很小的范围里找
    const int levels = 3, factor = 1 << levels;
    cv::Mat smallScene = bigScene, smallTempl = bigTempl;
    for (int i = 0; i < levels; ++i) {
        cv::pyrDown(smallScene, smallScene);
        cv::pyrDown(smallTempl, smallTempl);
    }
    const Match coarse = bestMatch(smallScene, smallTempl);
    // 大致位置换算回原图，取一个“模板大小 + 四周各 2×factor 个像素”的小窗口
    const int margin = 2 * factor;
    const cv::Rect window = cv::Rect(cvRound(coarse.x * factor - bigTempl.cols / 2.0) - margin, cvRound(coarse.y * factor - bigTempl.rows / 2.0) - margin,
                                     bigTempl.cols + 2 * margin, bigTempl.rows + 2 * margin) &
                            cv::Rect(0, 0, bigScene.cols, bigScene.rows);
    Match fine = bestMatch(bigScene(window), bigTempl);
    fine.x += window.x;
    fine.y += window.y;
    const auto t2 = now();

    std::cout << "大图 " << bigScene.cols << "×" << bigScene.rows << "：直接匹配 " << ms(t0, t1) << " 毫秒，位置 (" << direct.x << ", " << direct.y
              << ")；由粗到细 " << ms(t1, t2) << " 毫秒，位置 (" << fine.x << ", " << fine.y << ")\n";
    std::cout << "第 3 层上的粗略位置换算回原图是 (" << coarse.x * factor << ", " << coarse.y * factor << ")\n";
    c.check(std::hypot(fine.x - direct.x, fine.y - direct.y) < 1.0, "由粗到细得到的位置和直接匹配相同");

    std::cout << "结果图像保存在：" << out.string() << "\n";
    return c.exitCode();
}
