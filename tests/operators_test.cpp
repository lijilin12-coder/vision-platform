// 算子的测试：直接创建算子、喂给它合成的图像、检查输出。不经过方案和检测引擎。
// 每个算子至少测三件事：正常情况下算得对；参数起作用；输入不合适时给出看得懂的错误而不是崩溃。
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <opencv2/imgproc.hpp>

#include "vision/catalog/api.h"
#include "vision/imaging/cv_bridge.h"

using namespace vision;

namespace {

Image gray(const cv::Mat& m) { return imaging::fromMat(m).value(); }

Region regionOf(const cv::Mat& mask) { return Region::fromMask(gray(mask)).value(); }

// 运行一个算子：参数先取默认值，再用 overrides 覆盖
struct OpRun {
    Status status = Status::success();
    std::map<std::string, Value> outputs;

    template <typename T>
    const T& get(const std::string& port) const { return std::get<T>(outputs.at(port)); }
    cv::Mat image(const std::string& port = "image") const { return imaging::view(get<Image>(port)).clone(); }
    cv::Mat mask(const std::string& port = "region") const { return imaging::view(get<Region>(port).mask()).clone(); }
};

OpRun run(const std::string& id, const std::map<std::string, Value>& inputs, const ParamMap& overrides = {},
          std::optional<Roi> roi = std::nullopt, const std::map<std::string, MatchTemplateData>& templates = {}) {
    const auto& catalog = catalog::OperatorCatalog::global();
    const auto meta = catalog.find(id);
    EXPECT_TRUE(meta.has_value()) << id;
    ParamMap params = meta->signature.defaultParams();
    for (const auto& [k, v] : overrides) {
        const ParamDef* def = meta->signature.findParam(k);
        EXPECT_NE(def, nullptr) << id << " 没有参数 " << k;
        if (def != nullptr) {
            EXPECT_TRUE(def->validate(v).ok()) << id << "." << k;
        }
        params[k] = v;
    }
    auto op = catalog.create(id, meta->signature.majorVersion);
    EXPECT_TRUE(op.ok());
    catalog::OpContext ctx(inputs, params, std::move(roi), &templates);
    OpRun r;
    r.status = op.value()->execute(ctx);
    r.outputs = ctx.outputs();
    return r;
}

// 深色背景（60）上一个亮的矩形（200）
cv::Mat brightBox(int left, int width, int rows = 100, int cols = 300) {
    cv::Mat m(rows, cols, CV_8UC1, cv::Scalar(60));
    cv::rectangle(m, cv::Rect(left, 20, width, rows - 40), cv::Scalar(200), cv::FILLED);
    return m;
}

void addNoise(cv::Mat& m, double sigma, int seed = 7) {
    cv::RNG rng(seed);
    cv::Mat noise(m.size(), CV_16SC1);
    rng.fill(noise, cv::RNG::NORMAL, 0.0, sigma);
    cv::Mat wide;
    m.convertTo(wide, CV_16S);
    wide += noise;
    wide.convertTo(m, CV_8U);
}

}  // namespace

TEST(Operators, AllRegisteredWithoutErrors) {
    const auto& catalog = catalog::OperatorCatalog::global();
    EXPECT_TRUE(catalog.registrationErrors().empty());
    for (const char* id : {"vision.blur", "vision.normalize_brightness", "vision.adaptive_threshold", "vision.morphology",
                           "vision.edge_magnitude", "vision.region_area", "vision.blob_stats", "vision.caliper_width",
                           "vision.judge_range", "vision.template_match", "vision.align_to_pose"}) {
        const auto meta = catalog.find(id);
        ASSERT_TRUE(meta.has_value()) << id;
        EXPECT_FALSE(meta->displayName.empty()) << id;
        EXPECT_FALSE(meta->description.empty()) << id;
        // 每个参数的默认值都满足它自己声明的范围
        for (const auto& p : meta->signature.params) EXPECT_TRUE(p.validate(p.defaultValue).ok()) << id << "." << p.name;
    }
}

// ---------------- 平滑 ----------------

TEST(BlurOp, ReducesNoiseAndKeepsSizeAndFormat) {
    cv::Mat m(100, 100, CV_8UC1, cv::Scalar(120));
    addNoise(m, 20);
    cv::Scalar mean, before, after;
    cv::meanStdDev(m, mean, before);
    for (const char* method : {"gaussian", "median", "mean"}) {
        const OpRun r = run("vision.blur", {{"image", gray(m)}}, {{"method", std::string(method)}, {"size", 7.0}});
        ASSERT_TRUE(r.status.ok()) << method;
        const cv::Mat out = r.image();
        EXPECT_EQ(out.size(), m.size());
        EXPECT_EQ(out.type(), CV_8UC1);
        cv::meanStdDev(out, mean, after);
        EXPECT_LT(after[0], before[0] / 2) << method;  // 噪声至少减半
        EXPECT_NEAR(mean[0], 120, 2) << method;        // 平均亮度不变
    }
}

TEST(BlurOp, MedianRemovesIsolatedPixelsButKeepsEdges) {
    cv::Mat m = brightBox(100, 100);
    m.at<uchar>(10, 10) = 255;  // 一个孤立的亮点
    const cv::Mat out = run("vision.blur", {{"image", gray(m)}}, {{"method", std::string("median")}, {"size", 5.0}}).image();
    EXPECT_EQ(out.at<uchar>(10, 10), 60);    // 亮点没了
    EXPECT_EQ(out.at<uchar>(50, 99), 60);    // 边缘左边还是背景
    EXPECT_EQ(out.at<uchar>(50, 100), 200);  // 边缘右边还是物体：边缘没有被模糊
}

TEST(BlurOp, SizeOneLeavesImageUnchangedAndColorImagesWork) {
    cv::Mat m = brightBox(100, 100);
    EXPECT_EQ(cv::countNonZero(run("vision.blur", {{"image", gray(m)}}, {{"size", 1.0}}).image() != m), 0);
    cv::Mat color;
    cv::cvtColor(m, color, cv::COLOR_GRAY2BGR);
    const OpRun r = run("vision.blur", {{"image", imaging::fromMat(color).value()}});
    ASSERT_TRUE(r.status.ok());
    EXPECT_EQ(r.image().channels(), 3);
    EXPECT_FALSE(run("vision.blur", {}).status.ok());  // 没有输入
}

// ---------------- 亮度归一化 ----------------

TEST(NormalizeBrightnessOp, CancelsGlobalBrightnessChange) {
    // 背景 150，上面有一个亮斑 230。把整幅图调暗、调亮之后再归一化，结果应该差不多一样。
    cv::Mat base(100, 100, CV_8UC1, cv::Scalar(150));
    cv::circle(base, cv::Point(50, 50), 6, cv::Scalar(230), cv::FILLED);
    for (const double lighting : {0.6, 0.8, 1.0, 1.1}) {
        cv::Mat m;
        base.convertTo(m, CV_8U, lighting);
        const OpRun r = run("vision.normalize_brightness", {{"image", gray(m)}}, {{"target", 120.0}});
        ASSERT_TRUE(r.status.ok());
        const cv::Mat out = r.image();
        EXPECT_NEAR(out.at<uchar>(5, 5), 120, 1) << lighting;                       // 背景回到目标值
        EXPECT_NEAR(out.at<uchar>(50, 50), 230.0 * 120 / 150, 3) << lighting;       // 亮斑和背景的比例不变
        EXPECT_NEAR(r.get<double>("gain"), 120.0 / (150 * lighting), 0.02) << lighting;
    }
}

TEST(NormalizeBrightnessOp, UsesMedianSoSmallBrightDefectsDoNotMatter) {
    cv::Mat clean(100, 100, CV_8UC1, cv::Scalar(100));
    cv::Mat defect = clean.clone();
    cv::rectangle(defect, cv::Rect(0, 0, 30, 30), cv::Scalar(255), cv::FILLED);  // 9% 的面积是很亮的缺陷
    const double g1 = run("vision.normalize_brightness", {{"image", gray(clean)}}).get<double>("gain");
    const double g2 = run("vision.normalize_brightness", {{"image", gray(defect)}}).get<double>("gain");
    EXPECT_DOUBLE_EQ(g1, g2);  // 换成平均值的话，这里会差 14%
}

TEST(NormalizeBrightnessOp, RoiAndErrors) {
    // 左半边 50，右半边 200。ROI 只取右半边时，按 200 归一化
    cv::Mat m(40, 100, CV_8UC1, cv::Scalar(50));
    m(cv::Rect(50, 0, 50, 40)).setTo(200);
    const OpRun r = run("vision.normalize_brightness", {{"image", gray(m)}}, {{"target", 100.0}}, Roi::rect(50, 0, 50, 40).value());
    ASSERT_TRUE(r.status.ok());
    EXPECT_DOUBLE_EQ(r.get<double>("gain"), 0.5);

    cv::Mat black(40, 40, CV_8UC1, cv::Scalar(0));
    EXPECT_FALSE(run("vision.normalize_brightness", {{"image", gray(black)}}).status.ok());  // 全黑：没法归一化
    cv::Mat color(40, 40, CV_8UC3, cv::Scalar(10, 20, 30));
    const OpRun bad = run("vision.normalize_brightness", {{"image", imaging::fromMat(color).value()}});
    ASSERT_FALSE(bad.status.ok());
    EXPECT_NE(bad.status.error().message.find("灰度"), std::string::npos);  // 提示要先转灰度
}

// ---------------- 自适应阈值 ----------------

TEST(AdaptiveThresholdOp, FindsSpotsUnderUnevenLightingWhereFixedThresholdCannot) {
    // 从左到右亮度从 60 渐变到 180；左边和右边各有一个比周围亮 40 的斑点
    cv::Mat m(100, 300, CV_8UC1);
    for (int x = 0; x < m.cols; ++x) m.col(x).setTo(60 + 120.0 * x / (m.cols - 1));
    for (const int cx : {40, 260}) {
        cv::Mat spot = m(cv::Rect(cx - 5, 45, 10, 10));
        spot += 40;
    }
    const OpRun r = run("vision.adaptive_threshold", {{"image", gray(m)}}, {{"blockSize", 41.0}, {"offset", 20.0}});
    ASSERT_TRUE(r.status.ok());
    const cv::Mat mask = r.mask();
    EXPECT_EQ(mask.at<uchar>(50, 40), 255);   // 暗的一侧的斑点（灰度约 116）
    EXPECT_EQ(mask.at<uchar>(50, 260), 255);  // 亮的一侧的斑点（灰度约 204）
    EXPECT_EQ(mask.at<uchar>(50, 150), 0);    // 中间的背景（灰度约 120，比左边的斑点还亮）
    EXPECT_EQ(mask.at<uchar>(50, 290), 0);    // 最亮的背景（灰度约 176）
    EXPECT_NEAR(cv::countNonZero(mask), 200, 20);  // 只有两个 10×10 的斑点
}

TEST(AdaptiveThresholdOp, InvertFindsDarkSpotsAndRoiLimitsTheArea) {
    cv::Mat m(100, 200, CV_8UC1, cv::Scalar(150));
    cv::rectangle(m, cv::Rect(30, 40, 10, 10), cv::Scalar(90), cv::FILLED);
    cv::rectangle(m, cv::Rect(150, 40, 10, 10), cv::Scalar(90), cv::FILLED);
    const ParamMap dark = {{"blockSize", 41.0}, {"offset", 20.0}, {"invert", true}};
    EXPECT_EQ(cv::countNonZero(run("vision.adaptive_threshold", {{"image", gray(m)}}, dark).mask()), 200);
    const cv::Mat left = run("vision.adaptive_threshold", {{"image", gray(m)}}, dark, Roi::rect(0, 0, 100, 100).value()).mask();
    EXPECT_EQ(cv::countNonZero(left), 100);  // ROI 外面的那个不算
    EXPECT_EQ(left.at<uchar>(45, 155), 0);
    EXPECT_FALSE(run("vision.adaptive_threshold", {{"image", gray(m)}}, {}, Roi::rect(500, 500, 10, 10).value()).status.ok());
}

// ---------------- 形态学 ----------------

TEST(MorphologyOp, OpenRemovesSpecksCloseFillsHoles) {
    cv::Mat mask = cv::Mat::zeros(100, 100, CV_8UC1);
    cv::rectangle(mask, cv::Rect(20, 20, 40, 40), cv::Scalar(255), cv::FILLED);  // 一大块
    mask.at<uchar>(80, 80) = 255;                                                 // 一个噪点
    mask(cv::Rect(38, 38, 3, 3)).setTo(0);                                        // 大块中间的一个小洞

    const cv::Mat opened = run("vision.morphology", {{"region", regionOf(mask)}}, {{"operation", std::string("open")}, {"size", 5.0}}).mask();
    EXPECT_EQ(opened.at<uchar>(80, 80), 0);    // 噪点没了
    EXPECT_EQ(opened.at<uchar>(30, 30), 255);  // 大块还在

    const cv::Mat closed = run("vision.morphology", {{"region", regionOf(mask)}}, {{"operation", std::string("close")}, {"size", 7.0}}).mask();
    EXPECT_EQ(closed.at<uchar>(39, 39), 255);  // 洞填上了
    EXPECT_EQ(closed.at<uchar>(80, 80), 255);  // 闭运算不去噪点

    const int original = cv::countNonZero(mask);
    EXPECT_LT(cv::countNonZero(run("vision.morphology", {{"region", regionOf(mask)}}, {{"operation", std::string("erode")}}).mask()), original);
    EXPECT_GT(cv::countNonZero(run("vision.morphology", {{"region", regionOf(mask)}}, {{"operation", std::string("dilate")}}).mask()), original);
    EXPECT_FALSE(run("vision.morphology", {{"region", gray(mask)}}).status.ok());  // 给的是图像而不是区域
}

// ---------------- 边缘强度 ----------------

TEST(EdgeMagnitudeOp, BrightAtEdgesDarkOnFlatAreas) {
    const cv::Mat m = brightBox(100, 100);  // 两侧灰度差 140
    const OpRun r = run("vision.edge_magnitude", {{"image", gray(m)}});
    ASSERT_TRUE(r.status.ok());
    const cv::Mat out = r.image();
    EXPECT_EQ(out.at<uchar>(50, 50), 0);     // 背景里：平坦
    EXPECT_EQ(out.at<uchar>(50, 150), 0);    // 物体内部：平坦
    EXPECT_NEAR(out.at<uchar>(50, 100), 140, 5);  // 边缘上：约等于两侧的灰度差
    const cv::Mat doubled = run("vision.edge_magnitude", {{"image", gray(m)}}, {{"scale", 0.5}}).image();
    EXPECT_NEAR(doubled.at<uchar>(50, 100), 70, 3);
}

// ---------------- 区域面积、Blob 统计 ----------------

TEST(RegionAreaOp, CountsPixelsAndPercent) {
    cv::Mat mask = cv::Mat::zeros(100, 200, CV_8UC1);
    cv::rectangle(mask, cv::Rect(10, 10, 50, 20), cv::Scalar(255), cv::FILLED);
    const OpRun r = run("vision.region_area", {{"region", regionOf(mask)}});
    ASSERT_TRUE(r.status.ok());
    EXPECT_DOUBLE_EQ(r.get<double>("area"), 1000);
    EXPECT_DOUBLE_EQ(r.get<double>("percent"), 5.0);
    // ROI 只盖住一半
    const OpRun half = run("vision.region_area", {{"region", regionOf(mask)}}, {}, Roi::rect(0, 0, 35, 100).value());
    EXPECT_DOUBLE_EQ(half.get<double>("area"), 500);
    EXPECT_DOUBLE_EQ(run("vision.region_area", {{"region", regionOf(cv::Mat::zeros(10, 10, CV_8UC1))}}).get<double>("area"), 0);
}

TEST(BlobStatsOp, SumsAndFindsTheLargest) {
    BlobList blobs(3);
    blobs[0].area = 10;
    blobs[1].area = 250;
    blobs[2].area = 40;
    const OpRun r = run("vision.blob_stats", {{"blobs", blobs}});
    ASSERT_TRUE(r.status.ok());
    EXPECT_DOUBLE_EQ(r.get<double>("count"), 3);
    EXPECT_DOUBLE_EQ(r.get<double>("totalArea"), 300);
    EXPECT_DOUBLE_EQ(r.get<double>("maxArea"), 250);
    const OpRun empty = run("vision.blob_stats", {{"blobs", BlobList{}}});
    EXPECT_DOUBLE_EQ(empty.get<double>("count"), 0);
    EXPECT_DOUBLE_EQ(empty.get<double>("maxArea"), 0);
}

// ---------------- 卡尺测宽 ----------------

TEST(CaliperWidthOp, MeasuresWidthOfAnObject) {
    const OpRun r = run("vision.caliper_width", {{"image", gray(brightBox(80, 120))}});
    ASSERT_TRUE(r.status.ok());
    EXPECT_TRUE(r.get<bool>("found"));
    EXPECT_NEAR(r.get<double>("width"), 120, 0.5);
    EXPECT_NEAR(r.get<double>("edge1"), 79.5, 0.5);   // 边缘在第 79 和第 80 个像素之间
    EXPECT_NEAR(r.get<double>("edge2"), 199.5, 0.5);
}

// 亚像素：物体的边缘落在像素中间时（边缘像素的灰度是两侧的混合），测出来的宽度也应该带小数
TEST(CaliperWidthOp, SubpixelAccuracy) {
    for (const double width : {100.0, 100.25, 100.5, 100.75}) {
        cv::Mat m(60, 300, CV_8UC1, cv::Scalar(60));
        const double left = 90.0, right = left + width;
        for (int x = 0; x < m.cols; ++x) {
            // 像素 x 覆盖 [x, x+1)；它被物体盖住的比例决定它的灰度
            const double covered = std::max(0.0, std::min(x + 1.0, right) - std::max<double>(x, left));
            m.col(x).setTo(60 + 140 * covered);
        }
        const OpRun r = run("vision.caliper_width", {{"image", gray(m)}}, {{"smooth", 1.0}});
        ASSERT_TRUE(r.get<bool>("found"));
        EXPECT_NEAR(r.get<double>("width"), width, 0.02) << "实际宽度 " << width;
    }
}

TEST(CaliperWidthOp, WorksWithNoiseAndIgnoresWeakEdges) {
    cv::Mat m = brightBox(80, 120);
    addNoise(m, 8);
    const OpRun r = run("vision.caliper_width", {{"image", gray(m)}}, {{"edgeThreshold", 30.0}});
    ASSERT_TRUE(r.get<bool>("found"));
    EXPECT_NEAR(r.get<double>("width"), 120, 1.0);

    // 阈值比物体的对比度还高：找不到边缘。这不算出错——由后面的判定算子去判不合格
    const OpRun none = run("vision.caliper_width", {{"image", gray(m)}}, {{"edgeThreshold", 250.0}});
    ASSERT_TRUE(none.status.ok());
    EXPECT_FALSE(none.get<bool>("found"));
    EXPECT_DOUBLE_EQ(none.get<double>("width"), 0);

    // 一张什么都没有的图
    cv::Mat flat(60, 200, CV_8UC1, cv::Scalar(100));
    EXPECT_FALSE(run("vision.caliper_width", {{"image", gray(flat)}}).get<bool>("found"));
}

// 回归：边缘落在像素中间、再加上噪声时，导数曲线在一条边缘上会有两个相邻的小峰。
// 早先的实现把它们当成两条边缘，量出来的宽度多了 3～4 个像素（合格件被判成不合格）。
TEST(CaliperWidthOp, OneEdgeIsNotCountedTwice) {
    for (int seed = 1; seed <= 40; ++seed) {
        const double width = 396.0 + 0.2 * seed, left = 110.3 + 0.37 * seed, right = left + width;
        cv::Mat m(120, 640, CV_8UC1, cv::Scalar(150));
        for (int x = 0; x < m.cols; ++x) {
            const double covered = std::max(0.0, std::min(x + 1.0, right) - std::max<double>(x, left));
            m.col(x).setTo(150 - 75 * covered);
        }
        addNoise(m, 4, seed);
        const OpRun r = run("vision.caliper_width", {{"image", gray(m)}}, {{"edgeThreshold", 20.0}});
        ASSERT_TRUE(r.get<bool>("found")) << seed;
        EXPECT_NEAR(r.get<double>("width"), width, 0.3) << "seed " << seed;
    }
}

TEST(CaliperWidthOp, VerticalDirectionAndRoi) {
    cv::Mat m(200, 100, CV_8UC1, cv::Scalar(60));
    cv::rectangle(m, cv::Rect(20, 50, 60, 90), cv::Scalar(200), cv::FILLED);  // 高 90
    const OpRun vertical = run("vision.caliper_width", {{"image", gray(m)}}, {{"direction", std::string("vertical")}});
    ASSERT_TRUE(vertical.get<bool>("found"));
    EXPECT_NEAR(vertical.get<double>("width"), 90, 0.5);

    // 画面里有两个物体：用 ROI 只量右边那个；边缘位置是整幅图像里的坐标
    cv::Mat two(60, 400, CV_8UC1, cv::Scalar(60));
    cv::rectangle(two, cv::Rect(30, 10, 50, 40), cv::Scalar(200), cv::FILLED);
    cv::rectangle(two, cv::Rect(250, 10, 80, 40), cv::Scalar(200), cv::FILLED);
    const OpRun right = run("vision.caliper_width", {{"image", gray(two)}}, {}, Roi::rect(200, 0, 200, 60).value());
    ASSERT_TRUE(right.get<bool>("found"));
    EXPECT_NEAR(right.get<double>("width"), 80, 0.5);
    EXPECT_NEAR(right.get<double>("edge1"), 249.5, 0.5);
    // 不用 ROI 时量的是最外面的两条边：从左边物体的左边缘到右边物体的右边缘
    EXPECT_NEAR(run("vision.caliper_width", {{"image", gray(two)}}).get<double>("width"), 300, 0.5);
}

// ---------------- 范围判定 ----------------

TEST(JudgeRangeOp, PassesInsideRangeWithReadableReason) {
    const ParamMap params = {{"min", 150.0}, {"max", 155.0}, {"label", std::string("宽度")}, {"unit", std::string("像素")}};
    const OpRun ok = run("vision.judge_range", {{"value", 152.3}}, params);
    ASSERT_TRUE(ok.status.ok());
    EXPECT_TRUE(ok.get<Judgement>("judgement").pass);
    EXPECT_EQ(ok.get<Judgement>("judgement").reason, "宽度 152.3 像素，允许 150 ~ 155");

    EXPECT_FALSE(run("vision.judge_range", {{"value", 155.01}}, params).get<Judgement>("judgement").pass);
    EXPECT_FALSE(run("vision.judge_range", {{"value", 149.0}}, params).get<Judgement>("judgement").pass);
    EXPECT_TRUE(run("vision.judge_range", {{"value", 150.0}}, params).get<Judgement>("judgement").pass);  // 两端都算合格
    EXPECT_TRUE(run("vision.judge_range", {{"value", 155.0}}, params).get<Judgement>("judgement").pass);

    // 没有单位时不多出空格
    EXPECT_EQ(run("vision.judge_range", {{"value", 3.0}}, {{"min", 0.0}, {"max", 5.0}}).get<Judgement>("judgement").reason,
              "数值 3，允许 0 ~ 5");
    EXPECT_FALSE(run("vision.judge_range", {{"value", 3.0}}, {{"min", 9.0}, {"max", 5.0}}).status.ok());  // 最小值大于最大值
    EXPECT_FALSE(run("vision.judge_range", {{"value", std::string("3")}}).status.ok());                   // 给的不是数字
}

// ---------------- 定位：模板匹配、按位姿对齐（docs/ddd/09-match-template-and-locating.md）----------------

namespace {

// 样图：浅色背景上一块深色零件，零件上有几个不对称的特征（一个亮的圆、一个小方块、一道斜线）。
// 零件的宽度是 width 像素（用来检验对齐之后卡尺量得准不准）。
cv::Mat partImage(double width = 300.0, cv::Size size = cv::Size(640, 480)) {
    cv::Mat m(size, CV_8UC1, cv::Scalar(150));
    const double sx = size.width / 640.0, sy = size.height / 480.0;
    const double left = 170 * sx, right = left + width * sx;
    for (int x = 0; x < m.cols; ++x) {  // 左右边缘可以落在像素中间
        const double covered = std::max(0.0, std::min(x + 1.0, right) - std::max<double>(x, left));
        if (covered > 0) m(cv::Rect(x, cvRound(150 * sy), 1, cvRound(180 * sy))).setTo(150 - 80 * covered);
    }
    cv::circle(m, cv::Point(cvRound(230 * sx), cvRound(210 * sy)), cvRound(18 * sx), cv::Scalar(225), cv::FILLED, cv::LINE_AA);
    cv::rectangle(m, cv::Rect(cvRound(265 * sx), cvRound(225 * sy), cvRound(22 * sx), cvRound(14 * sy)), cv::Scalar(35), cv::FILLED);
    cv::line(m, cv::Point(cvRound(200 * sx), cvRound(250 * sy)), cv::Point(cvRound(250 * sx), cvRound(235 * sy)), cv::Scalar(200),
             std::max(1, cvRound(3 * sx)), cv::LINE_AA);
    return m;
}

// 框模板的位置：圆和小方块那一带（特征多、不对称）
const cv::Rect kTemplateBox(196, 176, 104, 88);

MatchTemplateData templateFrom(const cv::Mat& sample, cv::Rect box = kTemplateBox) {
    MatchTemplateData t;
    t.image = gray(sample(box).clone());
    t.referenceX = box.x;
    t.referenceY = box.y;
    t.sourceWidth = sample.cols;
    t.sourceHeight = sample.rows;
    return t;
}

// 把样图里的零件绕 pivot 顺时针转 angle 度，再平移 (dx, dy)；加噪声。同时算出模板中心现在的真实位置。
struct Moved {
    cv::Mat image;
    double x = 0, y = 0;  // 模板中心（基准点）现在的位置
};

Moved movePart(const cv::Mat& sample, double dx, double dy, double angle, int seed, cv::Rect box = kTemplateBox, double noise = 3.0) {
    const cv::Point2d pivot(sample.cols / 2.0, sample.rows / 2.0);
    const double radians = angle * CV_PI / 180.0, c = std::cos(radians), s = std::sin(radians);
    // 图像坐标 y 向下：这个矩阵在屏幕上看是顺时针转
    const cv::Mat m = (cv::Mat_<double>(2, 3) << c, -s, pivot.x - (c * pivot.x - s * pivot.y) + dx,
                       s, c, pivot.y - (s * pivot.x + c * pivot.y) + dy);
    Moved out;
    cv::warpAffine(sample, out.image, m, sample.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(150));
    if (noise > 0) addNoise(out.image, noise, seed);
    const double rx = box.x + box.width / 2.0, ry = box.y + box.height / 2.0;
    out.x = c * rx - s * ry + m.at<double>(0, 2);
    out.y = s * rx + c * ry + m.at<double>(1, 2);
    return out;
}

OpRun match(const cv::Mat& scene, const MatchTemplateData& t, const ParamMap& params = {}, std::optional<Roi> roi = std::nullopt) {
    return run("vision.template_match", {{"image", gray(scene)}}, params, std::move(roi), {{"template", t}});
}

}  // namespace

TEST(TemplateMatchOp, FindsThePartWhereItWasInTheSample) {
    const cv::Mat sample = partImage();
    const OpRun r = match(sample, templateFrom(sample));
    ASSERT_TRUE(r.status.ok()) << r.status.error().message;
    EXPECT_TRUE(r.get<bool>("found"));
    EXPECT_GT(r.get<double>("score"), 0.99);
    EXPECT_LE(r.get<double>("score"), 1.0);  // 回归：完全一样的图像曾经算出 1.0000001，被后面的范围判定判成超出范围
    const Pose p = r.get<Pose>("pose");
    EXPECT_NEAR(p.x, kTemplateBox.x + kTemplateBox.width / 2.0, 0.1);   // 基准点就在样图上的位置
    EXPECT_NEAR(p.y, kTemplateBox.y + kTemplateBox.height / 2.0, 0.1);
    EXPECT_NEAR(p.angle, 0, 0.1);
    EXPECT_DOUBLE_EQ(p.referenceX, kTemplateBox.x + kTemplateBox.width / 2.0);
    EXPECT_DOUBLE_EQ(p.referenceY, kTemplateBox.y + kTemplateBox.height / 2.0);
}

// 零件平移、旋转之后：位置和角度都找得准；角度的方向是“顺时针为正”
TEST(TemplateMatchOp, FindsShiftedAndRotatedPart) {
    const cv::Mat sample = partImage();
    const MatchTemplateData t = templateFrom(sample);
    struct Case { double dx, dy, angle; };
    for (const Case& c : {Case{25, -18, 0}, Case{-30.4, 12.7, 6.5}, Case{14.2, 22.3, -11.0}, Case{0, 0, 14.5}, Case{-8.6, -25.1, -3.2}}) {
        const Moved moved = movePart(sample, c.dx, c.dy, c.angle, 11);
        const OpRun r = match(moved.image, t);
        ASSERT_TRUE(r.status.ok());
        ASSERT_TRUE(r.get<bool>("found")) << "dx " << c.dx << " dy " << c.dy << " 角度 " << c.angle;
        const Pose p = r.get<Pose>("pose");
        // 容差按实测定的（见 LocatingMeasurement）：位置在 0.5 像素左右，角度在 0.7° 左右
        EXPECT_NEAR(p.x, moved.x, 0.8) << "角度 " << c.angle;
        EXPECT_NEAR(p.y, moved.y, 0.8) << "角度 " << c.angle;
        EXPECT_NEAR(p.angle, c.angle, 1.0) << "角度 " << c.angle;
    }
}

TEST(TemplateMatchOp, AngleRangeLimitsTheSearch) {
    const cv::Mat sample = partImage();
    const MatchTemplateData t = templateFrom(sample);
    const Moved moved = movePart(sample, 10, 5, 12, 3);
    EXPECT_TRUE(match(moved.image, t, {{"angleRange", 15.0}}).get<bool>("found"));
    // 只允许不旋转：转了 12 度的零件相似程度不够，算没找到
    const OpRun fixed = match(moved.image, t, {{"angleRange", 0.0}, {"minScore", 0.9}});
    ASSERT_TRUE(fixed.status.ok());
    EXPECT_FALSE(fixed.get<bool>("found"));
    // 整圈搜索也能找到，角度在 (-180, 180] 里
    const Moved upsideDown = movePart(sample, 0, 0, 170, 4);
    const OpRun full = match(upsideDown.image, t, {{"angleRange", 180.0}});
    ASSERT_TRUE(full.get<bool>("found"));
    EXPECT_NEAR(full.get<Pose>("pose").angle, 170, 1.0);
}

// 没找到不算出错：found 为 false，位姿取基准位置
TEST(TemplateMatchOp, NotFoundIsNotAnError) {
    const cv::Mat sample = partImage();
    cv::Mat empty(sample.size(), CV_8UC1, cv::Scalar(150));
    addNoise(empty, 3);
    const OpRun r = match(empty, templateFrom(sample));
    ASSERT_TRUE(r.status.ok());
    EXPECT_FALSE(r.get<bool>("found"));
    EXPECT_LT(r.get<double>("score"), 0.7);
    EXPECT_GE(r.get<double>("score"), 0.0);
    const Pose p = r.get<Pose>("pose");
    EXPECT_DOUBLE_EQ(p.x, p.referenceX);  // 没找到时不乱给位置：对齐算子拿到它等于什么都不做
    EXPECT_DOUBLE_EQ(p.angle, 0);
}

TEST(TemplateMatchOp, RoiRestrictsTheSearchAndPositionsStayInImageCoordinates) {
    const cv::Mat sample = partImage();
    const MatchTemplateData t = templateFrom(sample);
    const OpRun inside = match(sample, t, {}, Roi::rect(150, 130, 250, 200).value());
    ASSERT_TRUE(inside.get<bool>("found"));
    EXPECT_NEAR(inside.get<Pose>("pose").x, kTemplateBox.x + kTemplateBox.width / 2.0, 0.2);  // 仍然是整幅图像里的坐标
    // ROI 里没有零件
    EXPECT_FALSE(match(sample, t, {}, Roi::rect(400, 300, 200, 150).value()).get<bool>("found"));
    // ROI 比模板还小：没法找，算没找到，不崩溃
    const OpRun tiny = match(sample, t, {}, Roi::rect(200, 180, 40, 40).value());
    ASSERT_TRUE(tiny.status.ok());
    EXPECT_FALSE(tiny.get<bool>("found"));
}

// 模板是按像素定义的：图像的尺寸和框模板时的样图不一样，直接报错并说明原因
TEST(TemplateMatchOp, RejectsImagesOfADifferentSize) {
    const cv::Mat sample = partImage();
    const OpRun r = match(partImage(300, cv::Size(1280, 960)), templateFrom(sample));
    ASSERT_FALSE(r.status.ok());
    EXPECT_NE(r.status.error().message.find("640×480"), std::string::npos);
    EXPECT_NE(r.status.error().message.find("1280×960"), std::string::npos);

    // 没有设置模板、输入不是灰度图像
    EXPECT_FALSE(run("vision.template_match", {{"image", gray(sample)}}).status.ok());
    cv::Mat color;
    cv::cvtColor(sample, color, cv::COLOR_GRAY2BGR);
    EXPECT_FALSE(run("vision.template_match", {{"image", imaging::fromMat(color).value()}}, {}, std::nullopt,
                     {{"template", templateFrom(sample)}}).status.ok());
}

// 对齐：零件被移动、旋转之后，按位姿对齐回去，应该和样图差不多一样
TEST(AlignToPoseOp, BringsThePartBackToWhereItWasInTheSample) {
    const cv::Mat sample = partImage();
    const MatchTemplateData t = templateFrom(sample);
    const Moved moved = movePart(sample, 21.3, -14.6, 9.0, 5, kTemplateBox, 0.0);
    const Pose pose = match(moved.image, t).get<Pose>("pose");

    const OpRun r = run("vision.align_to_pose", {{"image", gray(moved.image)}, {"pose", pose}}, {{"fill", 150.0}});
    ASSERT_TRUE(r.status.ok());
    const cv::Mat aligned = r.image();
    EXPECT_EQ(aligned.size(), sample.size());
    // 零件所在的区域：对齐之后和样图几乎一样（边缘上因为重新采样会差一点）
    const cv::Rect part(150, 130, 340, 220);
    cv::Mat difference;
    cv::absdiff(aligned(part), sample(part), difference);
    EXPECT_LT(cv::mean(difference)[0], 1.5);
    // 没对齐的话差得很远
    cv::absdiff(moved.image(part), sample(part), difference);
    EXPECT_GT(cv::mean(difference)[0], 8.0);

    // 位姿就是基准位置（没有移动）时，对齐不改变图像
    Pose identity;
    identity.x = identity.referenceX = 248;
    identity.y = identity.referenceY = 220;
    const cv::Mat same = run("vision.align_to_pose", {{"image", gray(sample)}, {"pose", identity}}).image();
    EXPECT_EQ(cv::countNonZero(same != sample), 0);
    // 彩色图像也可以；缺了位姿报错
    cv::Mat color;
    cv::cvtColor(sample, color, cv::COLOR_GRAY2BGR);
    EXPECT_EQ(run("vision.align_to_pose", {{"image", imaging::fromMat(color).value()}, {"pose", pose}}).image().channels(), 3);
    EXPECT_FALSE(run("vision.align_to_pose", {{"image", gray(sample)}}).status.ok());
}

// 实测（数字写进 docs/poc/locating-report.md）：
//   1. 定位的位置误差和角度误差
//   2. 对齐之后再用卡尺量宽度，比直接在没有移动的图上量差多少
//   3. 耗时
TEST(LocatingMeasurement, AccuracyAndCost) {
    const double trueWidth = 300.4;
    const cv::Mat sample = partImage(trueWidth);
    const MatchTemplateData t = templateFrom(sample);
    const Roi band = Roi::rect(120, 290, 400, 30).value();  // 卡尺只量零件下半截（上面有圆和方块，会干扰）
    const double direct = run("vision.caliper_width", {{"image", gray(sample)}}, {}, band).get<double>("width");

    cv::RNG rng(2026);
    double maxPosition = 0, sumPosition = 0, maxAngle = 0, sumAngle = 0, maxWidth = 0, sumWidth = 0;
    const int n = 60;
    for (int i = 0; i < n; ++i) {
        const double dx = rng.uniform(-30.0, 30.0), dy = rng.uniform(-25.0, 25.0), angle = rng.uniform(-12.0, 12.0);
        const Moved moved = movePart(sample, dx, dy, angle, 100 + i);
        const OpRun r = match(moved.image, t);
        ASSERT_TRUE(r.get<bool>("found")) << i;
        const Pose p = r.get<Pose>("pose");
        const double positionError = std::hypot(p.x - moved.x, p.y - moved.y), angleError = std::abs(p.angle - angle);
        maxPosition = std::max(maxPosition, positionError);
        sumPosition += positionError;
        maxAngle = std::max(maxAngle, angleError);
        sumAngle += angleError;

        const cv::Mat aligned = run("vision.align_to_pose", {{"image", gray(moved.image)}, {"pose", p}}, {{"fill", 150.0}}).image();
        const OpRun caliper = run("vision.caliper_width", {{"image", gray(aligned)}}, {}, band);
        ASSERT_TRUE(caliper.get<bool>("found")) << i;
        const double widthError = std::abs(caliper.get<double>("width") - trueWidth);
        maxWidth = std::max(maxWidth, widthError);
        sumWidth += widthError;
    }
    std::cout << "[测量] 定位（" << n << " 次，平移 ±30 像素，旋转 ±12°，噪声 σ=3）：位置误差 平均 " << sumPosition / n << " 最大 "
              << maxPosition << " 像素；角度误差 平均 " << sumAngle / n << " 最大 " << maxAngle << "°\n";
    std::cout << "[测量] 卡尺测宽（真实 " << trueWidth << "）：不移动直接量 误差 " << std::abs(direct - trueWidth)
              << "；定位 + 对齐之后量 误差 平均 " << sumWidth / n << " 最大 " << maxWidth << " 像素\n";
    // 这几个上限只是为了发现“明显变差了”，不是精度指标；实测的数字见报告
    EXPECT_LT(maxPosition, 0.8);
    EXPECT_LT(maxAngle, 1.0);
    EXPECT_LT(maxWidth, 0.5);

    // 耗时：640×480 和 5MP，角度范围 ±15° 和整圈
    for (const cv::Size size : {cv::Size(640, 480), cv::Size(2592, 1944)}) {
        const cv::Mat big = partImage(300, size);
        const double scale = size.width / 640.0;
        const cv::Rect box(cvRound(kTemplateBox.x * scale), cvRound(kTemplateBox.y * scale), cvRound(kTemplateBox.width * scale),
                           cvRound(kTemplateBox.height * scale));
        const MatchTemplateData bigTemplate = templateFrom(big, box);
        const Moved moved = movePart(big, 20 * scale, -10 * scale, 7, 9, box);
        for (const double range : {15.0, 180.0}) {
            const auto t0 = std::chrono::steady_clock::now();
            const OpRun r = match(moved.image, bigTemplate, {{"angleRange", range}});
            const double matchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ASSERT_TRUE(r.get<bool>("found"));
            const auto t1 = std::chrono::steady_clock::now();
            (void)run("vision.align_to_pose", {{"image", gray(moved.image)}, {"pose", r.get<Pose>("pose")}});
            const double alignMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
            std::cout << "[测量] " << size.width << "×" << size.height << "，模板 " << box.width << "×" << box.height << "，角度 ±" << range
                      << "°：模板匹配 " << matchMs << " ms，对齐 " << alignMs << " ms\n";
        }
    }
    // 5MP 图像上换一个小模板（只框圆和方块那一带的一小块）：耗时主要由模板的大小决定，而不是图像的大小
    {
        const cv::Size size(2592, 1944);
        const cv::Mat big = partImage(300, size);
        // 框小方块那一带。不能只框那个圆：圆转多少度看起来都一样，角度就定不出来
        // （第一次试的时候框的就是圆的一部分：位置差了 2.4 像素，角度差了 9°，而且得分还很高）。
        const cv::Rect box(1040, 880, 150, 110);
        const MatchTemplateData small = templateFrom(big, box);
        const Moved moved = movePart(big, 80, -40, 7, 9, box);
        const auto t0 = std::chrono::steady_clock::now();
        const OpRun r = match(moved.image, small, {{"angleRange", 15.0}});
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ASSERT_TRUE(r.get<bool>("found"));
        const Pose p = r.get<Pose>("pose");
        EXPECT_LT(std::hypot(p.x - moved.x, p.y - moved.y), 1.0);
        EXPECT_LT(std::abs(p.angle - 7), 1.0);
        std::cout << "[测量] 2592×1944，小模板 150×110，角度 ±15°：模板匹配 " << ms << " ms；位置误差 "
                  << std::hypot(p.x - moved.x, p.y - moved.y) << " 像素，角度误差 " << std::abs(p.angle - 7) << "°\n";
    }
}
