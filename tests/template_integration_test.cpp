// 匹配模板机制的端到端测试（docs/ddd/09-match-template-and-locating.md，实施步骤 ②）。
//
// 这一步还没有定位算子。为了验证“模板能从方案一路传到算子手里”，这里注册了一个只在测试里存在的算子
// test.template_probe：它检查输入图像在基准位置上的那一块是不是和模板一模一样，一样就判合格。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>
#include <tuple>

#include "vision/authoring/api.h"
#include "vision/catalog/api.h"
#include "vision/common/path.h"
#include "vision/content/content_store.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/execution/api.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/persistence/json_repositories.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/release/api.h"
#include "vision/station/api.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

// ---------- 只在测试里存在的算子 ----------
class TemplateProbeOp final : public catalog::Operator {
public:
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "test.template_probe";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"judgement", PortType::Judgement, false}, {"width", PortType::Number, false},
                               {"referenceX", PortType::Number, false}, {"sourceWidth", PortType::Number, false}};
        m.signature.params = {{"template", ParamType::Template, std::string(""), std::nullopt, std::nullopt, {}, "匹配模板"}};
        m.displayName = "模板探针（测试用）";
        m.category = "测试";
        m.description = "输入图像在基准位置上的那一块和模板完全相同则合格";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Image>("image");
        if (!in) return in.error();
        auto t = ctx.matchTemplate("template");
        if (!t) return t.error();
        const cv::Mat image = imaging::view(in.value());
        const cv::Mat templ = imaging::view(t.value().image);
        const cv::Rect box(static_cast<int>(t.value().referenceX), static_cast<int>(t.value().referenceY), templ.cols, templ.rows);
        bool same = false;
        if ((box & cv::Rect(0, 0, image.cols, image.rows)) == box && image.type() == templ.type()) {
            same = cv::norm(image(box), templ, cv::NORM_INF) == 0;
        }
        ctx.setOutput("judgement", Judgement{same, same ? "基准位置上的图像和模板一致" : "基准位置上的图像和模板不一致"});
        ctx.setOutput("width", static_cast<double>(templ.cols));
        ctx.setOutput("referenceX", t.value().referenceX);
        ctx.setOutput("sourceWidth", static_cast<double>(t.value().sourceWidth));
        return Status::success();
    }
};

}  // namespace

VISION_REGISTER_OPERATOR(TemplateProbeOp)

namespace {

// 一张有图案的样图：每个位置的内容都不一样，所以“框的是哪一块”能分辨出来
Image sampleImage(int shift = 0) {
    cv::Mat m(120, 160, CV_8UC1);
    for (int y = 0; y < m.rows; ++y) {
        for (int x = 0; x < m.cols; ++x) m.at<uchar>(y, x) = static_cast<uchar>((x * 7 + y * 13 + shift) % 251);
    }
    return imaging::fromMat(m).value();
}

struct Platform {
    eventbus::InMemoryEventBus bus;
    std::vector<std::string> events;
    std::mutex eventsMutex;
    persistence::JsonRecipeRepository recipes;
    persistence::JsonRecipeVersionRepository versions;
    persistence::JsonReleaseRepository releaseRepository;
    persistence::JsonStationRepository stationRepository;
    content::FileContentStore store;
    content::TemplateContent templates;
    authoring::AuthoringService authoring;
    execution::ExecutionService execution;

    explicit Platform(const fs::path& dir)
        : recipes(dir),
          versions(dir),
          releaseRepository(dir),
          stationRepository(dir),
          store(dir),
          templates(store),
          authoring(recipes, versions, catalog::OperatorCatalog::global(), bus),
          execution(catalog::OperatorCatalog::global(), bus) {
        authoring.useTemplateStore(templates);
        execution.useTemplateSource(templates);
        bus.subscribe("*", [this](const EventPtr& e) {
            std::lock_guard<std::mutex> lock(eventsMutex);
            events.emplace_back(e->name());
        });
    }

    int count(const std::string& name) {
        std::lock_guard<std::mutex> lock(eventsMutex);
        return static_cast<int>(std::count(events.begin(), events.end(), name));
    }
};

class TemplateMechanism : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir = fs::temp_directory_path() / (std::string("vision-template-") + info->name());
        fs::remove_all(dir);
        p = std::make_unique<Platform>(dir);
    }
    void TearDown() override {
        p.reset();
        fs::remove_all(dir);
    }

    // 输入 → 模板探针（唯一的判定节点）。返回 {方案 ID, 探针节点 ID}
    std::pair<std::string, std::string> probeRecipe() {
        const auto recipe = p->authoring.createRecipe("模板测试").value();
        const auto input = p->authoring.addNode(recipe, "vision.image_input").value();
        const auto probe = p->authoring.addNode(recipe, "test.template_probe").value();
        EXPECT_TRUE(p->authoring.connect(recipe, input + ".image", probe + ".image").ok());
        return {recipe, probe};
    }

    // 建好方案，从样图上框 (40, 30, 48, 32) 作为模板并交给探针，发布。返回版本 ID
    std::string publishedWithTemplate(std::string* recipeOut = nullptr, std::string* probeOut = nullptr) {
        const auto [recipe, probe] = probeRecipe();
        const auto t = p->authoring.createMatchTemplate(recipe, "定位孔", sampleImage(), 40, 30, 48, 32);
        EXPECT_TRUE(t.ok()) << (t.ok() ? "" : t.error().message);
        EXPECT_TRUE(p->authoring.setParam(recipe, probe, "template", t.value()).ok());
        const auto version = p->authoring.publish(recipe);
        EXPECT_TRUE(version.ok()) << (version.ok() ? "" : version.error().message);
        if (recipeOut != nullptr) *recipeOut = recipe;
        if (probeOut != nullptr) *probeOut = probe;
        return version.value();
    }

    int filesInStore() const {
        int n = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(dir / "content", ec)) n += e.is_regular_file() ? 1 : 0;
        return n;
    }

    fs::path dir;
    std::unique_ptr<Platform> p;
};

}  // namespace

// ---------------- SHA-256 和内容仓库 ----------------

// 用标准文档（FIPS 180-4）给出的结果核对：哈希算错了，内容寻址就全错了
TEST(Sha256, MatchesTheStandardTestVectors) {
    const auto hash = [](const std::string& s) {
        return content::sha256Hex(reinterpret_cast<const unsigned char*>(s.data()), s.size());
    };
    EXPECT_EQ(hash(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hash("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(hash(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // 长度正好卡在补位的边界上（55、56、64 个字节）时也要对：这几个长度最容易写错
    EXPECT_EQ(hash(std::string(55, 'a')), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    EXPECT_EQ(hash(std::string(56, 'a')), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    EXPECT_EQ(hash(std::string(64, 'a')), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(FileContentStore, StoresByHashIdempotentlyAndDetectsCorruption) {
    const fs::path dir = fs::temp_directory_path() / "vision-content-store-test";
    fs::remove_all(dir);
    content::FileContentStore store(dir);
    const std::vector<unsigned char> bytes = {'a', 'b', 'c'};

    const auto hash = store.put(bytes);
    ASSERT_TRUE(hash.ok());
    EXPECT_EQ(hash.value(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_TRUE(store.contains(hash.value()));
    EXPECT_EQ(store.put(bytes).value(), hash.value());  // 再存一次：同一个哈希，不出错
    EXPECT_EQ(store.get(hash.value()).value(), bytes);

    EXPECT_FALSE(store.put({}).ok());                                  // 空内容
    EXPECT_EQ(store.get(std::string(64, '0')).error().code, "content.not_found");
    EXPECT_EQ(store.get("../../etc/passwd").error().code, "content.bad_hash");  // 不是哈希的东西不会被当成路径
    EXPECT_FALSE(store.contains("abc"));

    // 文件被改动：读取失败，而不是把错的内容交出去
    { std::ofstream(store.pathOf(hash.value()), std::ios::binary | std::ios::trunc) << "abd"; }
    EXPECT_EQ(store.get(hash.value()).error().code, "content.corrupt");
    fs::remove_all(dir);
}

// ---------------- 模板从方案一路传到算子 ----------------

TEST_F(TemplateMechanism, TemplateReachesTheOperatorThroughAPublishedVersion) {
    const auto version = publishedWithTemplate();
    const auto doc = p->authoring.getVersionDocument(version).value();

    // 发布语言：0.2，带着模板的引用（哈希、尺寸、基准位置、样图的尺寸），没有图像数据
    EXPECT_EQ(doc.schemaVersion, "0.2");
    ASSERT_EQ(doc.templates.size(), 1u);
    const auto& t = doc.templates[0];
    EXPECT_EQ(t.name, "定位孔");
    EXPECT_EQ(t.sha256.size(), 64u);
    EXPECT_GT(t.bytes, 0);
    EXPECT_EQ(t.width, 48);
    EXPECT_EQ(t.height, 32);
    EXPECT_EQ(t.referenceX, 40);
    EXPECT_EQ(t.referenceY, 30);
    EXPECT_EQ(t.sourceWidth, 160);
    EXPECT_EQ(t.sourceHeight, 120);
    EXPECT_TRUE(p->store.contains(t.sha256));

    // 算子拿到的模板和样图上框的那一块一模一样
    const auto same = p->execution.debugRun(doc, sampleImage()).value();
    EXPECT_EQ(same.report.verdict, "OK");
    const auto& outputs = same.outputs.begin()->second.count("width") ? same.outputs.begin()->second : std::next(same.outputs.begin())->second;
    EXPECT_EQ(std::get<double>(outputs.at("width")), 48);
    EXPECT_EQ(std::get<double>(outputs.at("referenceX")), 40);
    EXPECT_EQ(std::get<double>(outputs.at("sourceWidth")), 160);

    // 换一张内容不同的图：基准位置上的那一块和模板不一样
    EXPECT_EQ(p->execution.inspect(doc, sampleImage(5)).value().verdict, "NG");
}

// 模板随方案版本冻结：发布之后草稿里换模板、删模板，已发布的版本不受影响
TEST_F(TemplateMechanism, PublishedVersionKeepsItsTemplate) {
    std::string recipe, probe;
    const auto v1 = publishedWithTemplate(&recipe, &probe);
    const auto oldTemplate = p->authoring.getDraft(recipe).value().templates[0].id;

    // 草稿：从另一张图上框一个新模板，换上去，删掉旧的，发布 v2
    const auto t2 = p->authoring.createMatchTemplate(recipe, "新的", sampleImage(5), 40, 30, 48, 32).value();
    ASSERT_TRUE(p->authoring.setParam(recipe, probe, "template", t2).ok());
    ASSERT_TRUE(p->authoring.removeMatchTemplate(recipe, oldTemplate).ok());
    const auto v2 = p->authoring.publish(recipe).value();

    const auto doc1 = p->authoring.getVersionDocument(v1).value();
    const auto doc2 = p->authoring.getVersionDocument(v2).value();
    EXPECT_EQ(doc1.templates[0].name, "定位孔");
    EXPECT_NE(doc1.templates[0].sha256, doc2.templates[0].sha256);
    EXPECT_EQ(p->execution.inspect(doc1, sampleImage()).value().verdict, "OK");   // v1 用的还是旧模板
    EXPECT_EQ(p->execution.inspect(doc1, sampleImage(5)).value().verdict, "NG");
    EXPECT_EQ(p->execution.inspect(doc2, sampleImage(5)).value().verdict, "OK");  // v2 用新模板
    EXPECT_EQ(p->execution.inspect(doc2, sampleImage()).value().verdict, "NG");
}

// 同样的内容只存一份
TEST_F(TemplateMechanism, IdenticalContentIsStoredOnce) {
    const auto [recipe, probe] = probeRecipe();
    (void)probe;
    ASSERT_TRUE(p->authoring.createMatchTemplate(recipe, "甲", sampleImage(), 40, 30, 48, 32).ok());
    ASSERT_TRUE(p->authoring.createMatchTemplate(recipe, "乙", sampleImage(), 40, 30, 48, 32).ok());  // 同一块
    ASSERT_TRUE(p->authoring.createMatchTemplate(recipe, "丙", sampleImage(), 41, 30, 48, 32).ok());  // 偏了一个像素
    const auto draft = p->authoring.getDraft(recipe).value();
    ASSERT_EQ(draft.templates.size(), 3u);
    EXPECT_EQ(draft.templates[0].sha256, draft.templates[1].sha256);
    EXPECT_NE(draft.templates[0].sha256, draft.templates[2].sha256);
    EXPECT_EQ(filesInStore(), 2);
}

// 被拒绝的创建不留下任何东西
TEST_F(TemplateMechanism, RejectedCreationWritesNothing) {
    const auto [recipe, probe] = probeRecipe();
    (void)probe;
    EXPECT_FALSE(p->authoring.createMatchTemplate(recipe, "出界", sampleImage(), 130, 30, 48, 32).ok());  // 伸出了样图
    EXPECT_FALSE(p->authoring.createMatchTemplate(recipe, "太小", sampleImage(), 10, 10, 4, 4).ok());
    EXPECT_FALSE(p->authoring.createMatchTemplate(recipe, "", sampleImage(), 10, 10, 20, 20).ok());
    EXPECT_FALSE(p->authoring.createMatchTemplate("no-such-recipe", "x", sampleImage(), 10, 10, 20, 20).ok());
    ASSERT_TRUE(p->authoring.createMatchTemplate(recipe, "好的", sampleImage(), 10, 10, 20, 20).ok());
    EXPECT_FALSE(p->authoring.createMatchTemplate(recipe, "好的", sampleImage(), 50, 50, 20, 20).ok());  // 重名
    EXPECT_EQ(p->authoring.getDraft(recipe).value().templates.size(), 1u);
    EXPECT_EQ(filesInStore(), 1);
}

// 草稿存盘再读回来，模板还在；重启之后已发布的版本照样能用
TEST_F(TemplateMechanism, SurvivesRestart) {
    std::string recipe;
    const auto version = publishedWithTemplate(&recipe);
    p = std::make_unique<Platform>(dir);  // “重启”：全部服务重新建立，只留下磁盘上的文件

    const auto draft = p->authoring.getDraft(recipe).value();
    ASSERT_EQ(draft.templates.size(), 1u);
    EXPECT_EQ(draft.templates[0].name, "定位孔");
    const auto doc = p->authoring.getVersionDocument(version).value();
    EXPECT_EQ(p->execution.inspect(doc, sampleImage()).value().verdict, "OK");
    // 引用关系也读回来了：被节点使用的模板仍然不能删除
    EXPECT_FALSE(p->authoring.removeMatchTemplate(recipe, draft.templates[0].id).ok());
}

// ---------------- 出问题的时候 ----------------

// P6：模板参数没有设置的方案不能发布；调试运行时给出看得懂的错误
TEST_F(TemplateMechanism, UnsetTemplateBlocksPublishAndExplainsItselfInDebugRun) {
    const auto [recipe, probe] = probeRecipe();
    (void)probe;
    const auto rejected = p->authoring.publish(recipe);
    ASSERT_FALSE(rejected.ok());
    EXPECT_NE(rejected.error().message.find("模板参数"), std::string::npos);

    const auto run = p->execution.debugRun(p->authoring.getDraftDocument(recipe).value(), sampleImage());
    ASSERT_FALSE(run.ok());
    EXPECT_EQ(run.error().code, "execution.template_unset");
}

// 内容被改动或者丢失：加载方案时就报错，并且说得出是哪个模板——而不是用一幅错的模板继续检测
TEST_F(TemplateMechanism, CorruptOrMissingContentFailsAtLoadTime) {
    const auto version = publishedWithTemplate();
    const auto doc = p->authoring.getVersionDocument(version).value();
    const fs::path file = p->store.pathOf(doc.templates[0].sha256);
    ASSERT_TRUE(p->execution.prepare(doc).ok());

    p = std::make_unique<Platform>(dir);  // 重新建立，免得用到已经编译好的执行计划
    { std::ofstream(file, std::ios::binary | std::ios::app) << "x"; }  // 文件末尾多了一个字节
    const auto corrupt = p->execution.prepare(doc);
    ASSERT_FALSE(corrupt.ok());
    EXPECT_EQ(corrupt.error().code, "content.corrupt");
    EXPECT_NE(corrupt.error().message.find("定位孔"), std::string::npos);
    EXPECT_FALSE(p->execution.inspect(doc, sampleImage()).ok());

    fs::remove(file);
    const auto missing = p->execution.prepare(doc);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(missing.error().code, "content.not_found");
}

// 没有配置模板来源的程序遇到带模板的方案：明确报错
TEST_F(TemplateMechanism, EngineWithoutTemplateSourceRejectsRecipesThatNeedOne) {
    const auto version = publishedWithTemplate();
    const auto doc = p->authoring.getVersionDocument(version).value();
    execution::ExecutionService bare(catalog::OperatorCatalog::global(), p->bus);
    const auto run = bare.inspect(doc, sampleImage());
    ASSERT_FALSE(run.ok());
    EXPECT_EQ(run.error().code, "execution.no_template_source");
}

TEST_F(TemplateMechanism, AuthoringWithoutTemplateStoreCannotCreateTemplates) {
    authoring::AuthoringService bare(p->recipes, p->versions, catalog::OperatorCatalog::global(), p->bus);
    const auto recipe = bare.createRecipe("x").value();
    EXPECT_EQ(bare.createMatchTemplate(recipe, "t", sampleImage(), 10, 10, 20, 20).error().code, "authoring.no_template_store");
}

// 文档里的模板引用被篡改（指向一个不存在的模板）
TEST_F(TemplateMechanism, DanglingTemplateReferenceInDocumentIsRejected) {
    const auto version = publishedWithTemplate();
    auto doc = p->authoring.getVersionDocument(version).value();
    doc.versionId = "tampered@v1";
    doc.templates.clear();
    EXPECT_EQ(p->execution.prepare(doc).error().code, "execution.template_missing");
}

// ---------------- 发布语言的兼容 ----------------

// 0.1 的方案文档（没有 templates 这一段）照样能读、能运行
TEST_F(TemplateMechanism, OldSchemaDocumentsStillWork) {
    const char* v01 = R"({
      "schemaVersion": "0.1", "versionId": "old@v1", "recipeName": "旧格式",
      "nodes": [
        {"id": "n1", "operator": "vision.image_input", "operatorMajor": 1, "params": {}},
        {"id": "n2", "operator": "vision.to_gray", "operatorMajor": 1, "params": {}},
        {"id": "n3", "operator": "vision.threshold", "operatorMajor": 1,
         "params": {"method": "fixed", "value": 200, "invert": false}},
        {"id": "n4", "operator": "vision.blob", "operatorMajor": 1, "params": {"minArea": 1, "maxArea": 1000000000}},
        {"id": "n5", "operator": "vision.judge_count", "operatorMajor": 1, "params": {"maxCount": 0, "label": "亮斑"}}
      ],
      "edges": [{"from": "n1.image", "to": "n2.image"}, {"from": "n2.image", "to": "n3.image"},
                {"from": "n3.region", "to": "n4.region"}, {"from": "n4.blobs", "to": "n5.blobs"}]
    })";
    const auto doc = persistence::recipeDocumentFromJson(v01);
    ASSERT_TRUE(doc.ok()) << doc.error().message;
    EXPECT_EQ(doc.value().schemaVersion, "0.1");
    EXPECT_TRUE(doc.value().templates.empty());
    cv::Mat dark(60, 80, CV_8UC3, cv::Scalar(50, 50, 50));
    EXPECT_EQ(p->execution.inspect(doc.value(), imaging::fromMat(dark).value()).value().verdict, "OK");

    auto future = doc.value();
    future.schemaVersion = "9.9";
    future.versionId = "future@v1";
    EXPECT_EQ(p->execution.inspect(future, imaging::fromMat(dark).value()).error().code, "execution.schema");
}

TEST_F(TemplateMechanism, DocumentJsonRoundTripKeepsTemplates) {
    const auto version = publishedWithTemplate();
    const auto doc = p->authoring.getVersionDocument(version).value();
    const auto back = persistence::recipeDocumentFromJson(persistence::toJson(doc));
    ASSERT_TRUE(back.ok());
    ASSERT_EQ(back.value().templates.size(), 1u);
    EXPECT_EQ(back.value().templates[0].sha256, doc.templates[0].sha256);
    EXPECT_EQ(back.value().templates[0].referenceY, 30);
    EXPECT_EQ(back.value().templates[0].sourceHeight, 120);
    EXPECT_EQ(std::get<std::string>(back.value().nodes[1].params.at("template")), doc.templates[0].id);
    EXPECT_EQ(p->execution.inspect(back.value(), sampleImage()).value().verdict, "OK");
}

// ---------------- 上线和工位：不需要任何改动 ----------------

namespace {

class AuthoringSource final : public release::RecipeVersionSource {
public:
    explicit AuthoringSource(const authoring::AuthoringService& a) : authoring_(a) {}
    Result<authoring::RecipeVersionDocument> find(const std::string& id) const override { return authoring_.getVersionDocument(id); }

private:
    const authoring::AuthoringService& authoring_;
};

}  // namespace

// 带模板的方案组装成上线包、部署到工位：后台加载时模板一起准备好；内容损坏时加载失败，不影响正在生产的上线包
TEST_F(TemplateMechanism, DeploysToAStationAndABrokenTemplateDoesNotAffectProduction) {
    std::string recipe, probe;
    const auto v1 = publishedWithTemplate(&recipe, &probe);
    const auto t2 = p->authoring.createMatchTemplate(recipe, "新的", sampleImage(5), 40, 30, 48, 32).value();
    ASSERT_TRUE(p->authoring.setParam(recipe, probe, "template", t2).ok());
    const auto v2 = p->authoring.publish(recipe).value();

    AuthoringSource source(p->authoring);
    release::ReleaseService releases(p->releaseRepository, source, p->bus);
    station::StationService stations(p->stationRepository, releases, p->execution, p->bus);
    const auto r1 = releases.createRelease(v1).value();
    const auto r2 = releases.createRelease(v2).value();

    const auto id = stations.createStation("工位一", {3, 5}).value();
    ASSERT_TRUE(stations.deploy(id, r1).ok());
    ASSERT_TRUE(stations.waitForLoading(id).ok());
    ASSERT_TRUE(stations.start(id).ok());
    EXPECT_EQ(stations.inspect(id, sampleImage()).value().report.verdict, "OK");

    // 把 v2 实际用的那个模板（“新的”）的内容弄坏，再部署 v2。
    // （v2 里还留着没有被引用的旧模板“定位孔”；它没有被任何节点用到，坏不坏都不影响加载。）
    const auto doc2 = p->authoring.getVersionDocument(v2).value();
    const auto used = std::find_if(doc2.templates.begin(), doc2.templates.end(),
                                   [](const authoring::RecipeDocumentTemplate& t) { return t.name == "新的"; });
    ASSERT_NE(used, doc2.templates.end());
    { std::ofstream(p->store.pathOf(used->sha256), std::ios::binary | std::ios::trunc) << "broken"; }
    ASSERT_TRUE(stations.deploy(id, r2).ok());
    ASSERT_TRUE(stations.waitForLoading(id).ok());
    EXPECT_EQ(p->count("ReleaseLoadFailed"), 1);

    const auto still = stations.inspect(id, sampleImage()).value();
    EXPECT_EQ(still.releaseId, r1);  // 还在用原来的上线包
    EXPECT_FALSE(still.swapped);
    EXPECT_EQ(still.report.verdict, "OK");
    EXPECT_EQ(stations.getStation(id).value().state, "running");
}

// ---------------- 定位 + 对齐 + 测量：真正用到模板的方案，走完整个引擎 ----------------

namespace {

// 浅色背景上一块深色零件（宽 width），上面有一个亮的圆和一个小方块。整体平移 (dx, dy)、顺时针转 angle 度
Image locatingImage(double width, double dx, double dy, double angle) {
    cv::Mat m(240, 320, CV_8UC3, cv::Scalar(150, 150, 150));
    cv::rectangle(m, cv::Rect(90, 80, static_cast<int>(width), 90), cv::Scalar(70, 70, 70), cv::FILLED);
    cv::circle(m, cv::Point(125, 110), 10, cv::Scalar(225, 225, 225), cv::FILLED, cv::LINE_AA);
    cv::rectangle(m, cv::Rect(145, 118, 12, 8), cv::Scalar(30, 30, 30), cv::FILLED);
    const double radians = angle * CV_PI / 180.0, c = std::cos(radians), s = std::sin(radians);
    const cv::Mat move = (cv::Mat_<double>(2, 3) << c, -s, 160 - (c * 160 - s * 120) + dx, s, c, 120 - (s * 160 + c * 120) + dy);
    cv::Mat out;
    cv::warpAffine(m, out, move, m.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(150, 150, 150));
    return imaging::fromMat(out).value();
}

Image toGray(const Image& color) {
    cv::Mat g;
    cv::cvtColor(imaging::view(color), g, cv::COLOR_BGR2GRAY);
    return imaging::fromMat(g).value();
}

}  // namespace

// 输入 → 灰度 ─┬→ 模板匹配 ─(score)→ 范围判定「定位」
//              └────(pose)────→ 按位姿对齐 → 卡尺测宽 → 范围判定「宽度」
TEST_F(TemplateMechanism, LocateAlignAndMeasureEndToEnd) {
    auto& a = p->authoring;
    const auto recipe = a.createRecipe("定位后测宽").value();
    const auto input = a.addNode(recipe, "vision.image_input").value();
    const auto gray = a.addNode(recipe, "vision.to_gray").value();
    const auto match = a.addNode(recipe, "vision.template_match").value();
    const auto located = a.addNode(recipe, "vision.judge_range").value();
    const auto align = a.addNode(recipe, "vision.align_to_pose").value();
    const auto caliper = a.addNode(recipe, "vision.caliper_width").value();
    const auto widthOk = a.addNode(recipe, "vision.judge_range").value();
    ASSERT_TRUE(a.connect(recipe, input + ".image", gray + ".image").ok());
    ASSERT_TRUE(a.connect(recipe, gray + ".image", match + ".image").ok());
    ASSERT_TRUE(a.connect(recipe, match + ".score", located + ".value").ok());
    ASSERT_TRUE(a.connect(recipe, gray + ".image", align + ".image").ok());
    ASSERT_TRUE(a.connect(recipe, match + ".pose", align + ".pose").ok());
    ASSERT_TRUE(a.connect(recipe, align + ".image", caliper + ".image").ok());
    ASSERT_TRUE(a.connect(recipe, caliper + ".width", widthOk + ".value").ok());
    // 类型检查对位姿同样有效：位姿不能接到要图像的端口上，数值也不能接到要位姿的端口上
    EXPECT_FALSE(a.connect(recipe, match + ".pose", caliper + ".image").ok());
    EXPECT_FALSE(a.connect(recipe, match + ".score", align + ".pose").ok());

    ASSERT_TRUE(a.setParam(recipe, located, "min", 0.7).ok());
    ASSERT_TRUE(a.setParam(recipe, located, "max", 1.0).ok());
    ASSERT_TRUE(a.setParam(recipe, located, "label", std::string("定位得分")).ok());
    ASSERT_TRUE(a.setParam(recipe, widthOk, "min", 137.0).ok());
    ASSERT_TRUE(a.setParam(recipe, widthOk, "max", 143.0).ok());
    ASSERT_TRUE(a.setParam(recipe, widthOk, "label", std::string("宽度")).ok());
    ASSERT_TRUE(a.setParam(recipe, align, "fill", 150.0).ok());
    // 卡尺只量零件的下半截：ROI 是在“零件在标准位置”的图上画的，对齐之后才框得准
    ASSERT_TRUE(a.setRoi(recipe, caliper, Roi::rect(60, 140, 220, 24).value()).ok());

    // 模板要从灰度的样图上框：模板匹配算子处理的是灰度图像
    const auto sample = toGray(locatingImage(140, 0, 0, 0));
    const auto t = a.createMatchTemplate(recipe, "圆和方块", sample, 106, 92, 60, 44);
    ASSERT_TRUE(t.ok()) << t.error().message;
    ASSERT_TRUE(a.setParam(recipe, match, "template", t.value()).ok());
    const auto version = a.publish(recipe);
    ASSERT_TRUE(version.ok()) << version.error().message;
    const auto doc = a.getVersionDocument(version.value()).value();

    // 零件在标准位置、偏了、转了：都合格
    for (const auto& [dx, dy, angle] : std::vector<std::tuple<double, double, double>>{{0, 0, 0}, {14, -9, 0}, {-10, 7, 6}, {6, 5, -9}}) {
        const auto r = p->execution.inspect(doc, locatingImage(140, dx, dy, angle)).value();
        EXPECT_EQ(r.verdict, "OK") << "dx " << dx << " dy " << dy << " 角度 " << angle;
    }
    // 宽度超差的零件：即使转了，量出来的仍然是它真实的宽度，判不合格
    const auto wide = p->execution.inspect(doc, locatingImage(150, -10, 7, 6)).value();
    EXPECT_EQ(wide.verdict, "NG");
    bool widthReason = false;
    for (const auto& reason : wide.reasons) widthReason |= !reason.pass && reason.text.find("宽度") != std::string::npos;
    EXPECT_TRUE(widthReason);

    // 画面里没有零件：定位这一条判不合格，操作员看得到原因；不是程序出错
    cv::Mat blank(240, 320, CV_8UC3, cv::Scalar(150, 150, 150));
    const auto none = p->execution.inspect(doc, imaging::fromMat(blank).value()).value();
    EXPECT_EQ(none.verdict, "NG");
    bool locatingReason = false;
    for (const auto& reason : none.reasons) locatingReason |= !reason.pass && reason.text.find("定位得分") != std::string::npos;
    EXPECT_TRUE(locatingReason);

    // 换了分辨率的相机：明确报错（模板是按像素定义的），而不是给一个“没找到”
    cv::Mat bigger(480, 640, CV_8UC3, cv::Scalar(150, 150, 150));
    const auto mismatch = p->execution.inspect(doc, imaging::fromMat(bigger).value()).value();
    EXPECT_EQ(mismatch.verdict, "ERROR");

    // 调试运行能看到位姿
    const auto debug = p->execution.debugRun(doc, locatingImage(140, 14, -9, 0)).value();
    bool sawPose = false;
    for (const auto& n : debug.report.nodes) {
        const auto it = n.outputs.find("pose");
        if (it != n.outputs.end()) sawPose = it->second.rfind("pose (", 0) == 0;
    }
    EXPECT_TRUE(sawPose);

    // 存盘再读回来：位姿这种端口类型在方案文件里保存和恢复都没有问题
    p = std::make_unique<Platform>(dir);
    const auto again = p->authoring.getVersionDocument(version.value()).value();
    EXPECT_EQ(p->execution.inspect(again, locatingImage(140, -10, 7, 6)).value().verdict, "OK");
}
