// 算子的测试：直接创建算子、喂给它合成的图像、检查输出。不经过方案和检测引擎。
// 每个算子至少测三件事：正常情况下算得对；参数起作用；输入不合适时给出看得懂的错误而不是崩溃。
#include <gtest/gtest.h>

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
        std::optional<Roi> roi = std::nullopt) {
    const auto& catalog = catalog::OperatorCatalog::global();
    const auto meta = catalog.find(id);
    EXPECT_TRUE(meta.has_value()) << id;
    ParamMap params = meta->signature.defaultParams();
    for (const auto& [k, v] : overrides) {
        const ParamDef* def = meta->signature.findParam(k);
        EXPECT_NE(def, nullptr) << id << " 没有参数 " << k;
        if (def != nullptr) EXPECT_TRUE(def->validate(v).ok()) << id << "." << k;
        params[k] = v;
    }
    auto op = catalog.create(id, meta->signature.majorVersion);
    EXPECT_TRUE(op.ok());
    catalog::OpContext ctx(inputs, params, std::move(roi));
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
                           "vision.judge_range"}) {
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
