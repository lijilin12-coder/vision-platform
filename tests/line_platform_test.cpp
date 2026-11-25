// 产线运行平台（docs/ddd/13-two-programs.md）：
//   - 它的组装里没有方案编排：方案只能来自方案包
//   - serve：工位持续运行；运行期间通过控制接口热切换、回滚
//   - 命令行的转发：服务在运行时，对工位的操作交给它

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <opencv2/imgproc.hpp>
#include <thread>
#include <type_traits>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "line_api.h"
#include "line_commands.h"
#include "platform.h"
#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"

using namespace vision;
using nlohmann::json;
namespace fs = std::filesystem;

// 编译期的检查：产线运行平台的组装里没有方案编排、方案助手、数据、训练这些成员。
// （用“有没有这个成员”来查：谁往 LinePlatform 里加了它们，这里就编译不过。）
namespace {
template <typename T, typename = void> struct HasAuthoring : std::false_type {};
template <typename T> struct HasAuthoring<T, std::void_t<decltype(std::declval<T&>().authoring)>> : std::true_type {};
template <typename T, typename = void> struct HasTraining : std::false_type {};
template <typename T> struct HasTraining<T, std::void_t<decltype(std::declval<T&>().training)>> : std::true_type {};
template <typename T, typename = void> struct HasData : std::false_type {};
template <typename T> struct HasData<T, std::void_t<decltype(std::declval<T&>().data)>> : std::true_type {};
}  // namespace
static_assert(!HasAuthoring<platform::LinePlatform>::value, "产线运行平台不应该有方案编排");
static_assert(!HasTraining<platform::LinePlatform>::value, "产线运行平台不应该有训练");
static_assert(!HasData<platform::LinePlatform>::value, "产线运行平台不应该有数据集");
static_assert(HasAuthoring<platform::Platform>::value, "方案构建平台有方案编排");

namespace {

void writeImage(const fs::path& file, int spots) {
    cv::Mat m(120, 160, CV_8UC1, cv::Scalar(80));
    for (int i = 0; i < spots; ++i) cv::circle(m, {30 + 40 * i, 60}, 8, cv::Scalar(240), cv::FILLED);
    ASSERT_TRUE(imaging::saveImage(pathToUtf8(file), imaging::fromMat(m).value()).ok());
}

class LineProgram : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root = fs::temp_directory_path() / pathFromUtf8(std::string("vision-产线-") + info->name());
        fs::remove_all(root);
        fs::create_directories(root / "images");
        builder = std::make_unique<platform::Platform>(root / "builder");
        line = std::make_unique<platform::LinePlatform>(root / "line");
        line->stations.allowUnacceptedReleases(true);  // 这里测的不是验收：允许部署没验收的上线包（规则 L4 的测试在 acceptance_test.cpp、release_bundle_test.cpp）
    }
    void TearDown() override {
        line.reset();
        builder.reset();
        fs::remove_all(root);
    }

    // 在构建机上搭“亮斑不超过 maxCount 个”的方案，发布、导出；在产线机上导入、做成上线包。返回上线包 ID
    std::string releaseAllowing(int maxCount) {
        auto& a = builder->authoring;
        const auto recipe = a.createRecipe("亮斑 ≤ " + std::to_string(maxCount)).value();
        std::vector<std::string> n;
        for (const char* op : {"vision.image_input", "vision.to_gray", "vision.threshold", "vision.blob", "vision.judge_count"}) {
            n.push_back(a.addNode(recipe, op).value());
        }
        EXPECT_TRUE(a.connect(recipe, n[0] + ".image", n[1] + ".image").ok());
        EXPECT_TRUE(a.connect(recipe, n[1] + ".image", n[2] + ".image").ok());
        EXPECT_TRUE(a.connect(recipe, n[2] + ".region", n[3] + ".region").ok());
        EXPECT_TRUE(a.connect(recipe, n[3] + ".blobs", n[4] + ".blobs").ok());
        EXPECT_TRUE(a.setParam(recipe, n[2], "value", 200.0).ok());
        EXPECT_TRUE(a.setParam(recipe, n[4], "maxCount", static_cast<double>(maxCount)).ok());
        const auto version = a.publish(recipe).value();
        const std::string file = pathToUtf8(root / ("p" + std::to_string(maxCount) + ".vpkg"));
        EXPECT_TRUE(builder->packages.exportPackage(version, file).ok());
        const auto imported = line->packages.importPackage(file);
        EXPECT_TRUE(imported.ok()) << (imported.ok() ? "" : imported.error().message);
        const auto release = line->releases.createRelease(version);
        EXPECT_TRUE(release.ok()) << (release.ok() ? "" : release.error().message);
        return release.value();
    }

    // 产线机上的一个工位：部署好、绑定了图像目录
    std::string stationWith(const std::string& release) {
        const auto station = line->stations.createStation("一号").value();
        EXPECT_TRUE(line->stations.deploy(station, release).ok());
        EXPECT_TRUE(line->stations.waitForLoading(station).ok());
        EXPECT_TRUE(line->stations.bindImageSource(station, "folder", pathToUtf8(root / "images")).ok());
        return station;
    }

    fs::path root;
    std::unique_ptr<platform::Platform> builder;
    std::unique_ptr<platform::LinePlatform> line;
};

// 等到条件成立，最多等几秒
template <typename Fn>
bool eventually(Fn&& condition, int seconds = 10) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return condition();
}

}  // namespace

// 产线运行平台只认方案包带来的方案版本
TEST_F(LineProgram, RecipesOnlyArriveThroughPackages) {
    auto& a = builder->authoring;
    const auto recipe = a.createRecipe("没有导出的").value();
    const auto input = a.addNode(recipe, "vision.image_input").value();
    const auto gray = a.addNode(recipe, "vision.to_gray").value();
    const auto judge = a.addNode(recipe, "vision.judge_range").value();
    ASSERT_TRUE(a.connect(recipe, input + ".image", gray + ".image").ok());
    (void)judge;
    const auto release = releaseAllowing(0);
    EXPECT_FALSE(release.empty());
    EXPECT_EQ(line->importedRecipes.listIds().size(), 1u);
    // 构建机上发布了、但没有通过方案包带过来的方案版本：产线机不认识
    const auto unknown = line->releases.createRelease("11111111-2222-3333-4444-555555555555@v1");
    ASSERT_FALSE(unknown.ok());
    EXPECT_NE(unknown.error().message.find("导入"), std::string::npos);
}

// serve：工位持续运行，取完现有的图像后空闲一会儿自己结束
TEST_F(LineProgram, ServeRunsBoundStationsUntilIdle) {
    writeImage(root / "images" / "1.png", 0);
    writeImage(root / "images" / "2.png", 2);
    std::ofstream(root / "images" / "3.png") << "坏的";
    writeImage(root / "images" / "4.png", 1);
    const auto station = stationWith(releaseAllowing(0));
    std::atomic<bool> stop{false};
    EXPECT_EQ(cli::serveStations(*line, {}, /*exitWhenIdleSeconds=*/1, stop), 0);
    const auto view = line->stations.getStation(station).value();
    EXPECT_EQ(view.sequence, 3u);  // 三幅好的图像各检测了一次；坏的那幅不算
    EXPECT_EQ(view.state, "stopped");

    // 没有绑定图像源的工位：serve 没有可运行的
    ASSERT_TRUE(line->stations.unbindImageSource(station).ok());
    EXPECT_EQ(cli::serveStations(*line, {}, 1, stop), 1);
    // 指定了一个启动不了的工位（没有图像源也能启动，但没有上线包不行）
    const auto empty = line->stations.createStation("空的").value();
    EXPECT_EQ(cli::serveStations(*line, {empty}, 1, stop), 1);
}

// 运行期间通过控制接口热切换、回滚：不用停，正在运行的工位在下一次检测前换上新的上线包
TEST_F(LineProgram, HotSwapAndRollbackThroughTheControlApiWhileServing) {
    const auto strict = releaseAllowing(0), lenient = releaseAllowing(5);
    const auto station = stationWith(strict);
    writeImage(root / "images" / "a.png", 2);

    line::ControlApi api(*line);
    const int port = api.bind("127.0.0.1", 0);
    ASSERT_GT(port, 0);
    std::thread apiThread([&api] { api.serve(); });
    std::atomic<bool> stop{false};
    std::thread serving([&] { cli::serveStations(*line, {station}, 0, stop); });

    httplib::Client client("127.0.0.1", port);
    const auto get = [&](const std::string& path) {
        auto r = client.Get(path);
        return r ? std::make_pair(r->status, json::parse(r->body, nullptr, false)) : std::make_pair(0, json());
    };
    const auto post = [&](const std::string& path, const json& body) {
        auto r = client.Post(path, body.dump(), "application/json");
        return r ? std::make_pair(r->status, json::parse(r->body, nullptr, false)) : std::make_pair(0, json());
    };
    ASSERT_TRUE(eventually([&] { return get("/api/health").first == 200; }));
    const std::string base = "/api/stations/" + station;

    // 第一幅：严格的方案，2 个亮斑 → 检测过一次
    ASSERT_TRUE(eventually([&] { return get(base).second.value("sequence", 0) == 1; }));
    EXPECT_EQ(get(base).second["activeRelease"], strict);
    EXPECT_EQ(get(base).second["state"], "running");
    EXPECT_EQ(get("/api/stations").second["stations"].size(), 1u);

    // 部署宽松的方案：立即返回，后台加载
    const auto deployed = post(base + "/deploy", {{"releaseId", lenient}});
    ASSERT_EQ(deployed.first, 200) << deployed.second.dump();
    ASSERT_TRUE(eventually([&] { return get(base).second.value("pendingReady", false); }));
    EXPECT_EQ(get(base).second["activeRelease"], strict);  // 还没有检测，所以还没有切换（S3）
    // 下一幅图像到来：检测开始前切换
    writeImage(root / "images" / "b.png", 2);
    ASSERT_TRUE(eventually([&] { return get(base).second.value("sequence", 0) == 2; }));
    EXPECT_EQ(get(base).second["activeRelease"], lenient);
    EXPECT_EQ(get(base).second["pendingRelease"], "");

    // 回滚
    const auto rolled = post(base + "/rollback", json::object());
    ASSERT_EQ(rolled.first, 200) << rolled.second.dump();
    EXPECT_EQ(rolled.second["activeRelease"], strict);

    // 规则照常：不存在的上线包、不存在的工位、看不懂的请求
    EXPECT_EQ(post(base + "/deploy", {{"releaseId", "nope"}}).first, 404);
    EXPECT_EQ(post("/api/stations/nobody/deploy", {{"releaseId", lenient}}).first, 404);
    EXPECT_EQ(post(base + "/deploy", json::object()).first, 400);
    EXPECT_EQ(get("/api/recipes").first, 404);  // 产线运行平台的接口里没有方案编排

    stop = true;
    serving.join();
    api.stop();
    apiThread.join();
    EXPECT_EQ(line->stations.getStation(station).value().state, "stopped");
}

// 命令行的转发：靠数据目录里的记号文件找到正在运行的服务
TEST_F(LineProgram, CommandsFindTheRunningServiceThroughItsMarkerFile) {
    const fs::path data = root / "line";
    EXPECT_TRUE(line::runningService(data).empty());

    // 旧的记号文件（服务已经不在了）：不算在运行，而且被清掉
    line::writeServeMarker(data, 1);  // 1 号端口上不会有我们的服务
    EXPECT_TRUE(line::runningService(data).empty());
    EXPECT_FALSE(fs::exists(data / "serve.json"));
    std::ofstream(data / "serve.json") << "不是 JSON";
    EXPECT_TRUE(line::runningService(data).empty());

    const auto station = stationWith(releaseAllowing(0));
    line::ControlApi api(*line);
    const int port = api.bind("127.0.0.1", 0);
    std::thread apiThread([&api] { api.serve(); });
    line::writeServeMarker(data, port);
    std::string url;
    ASSERT_TRUE(eventually([&] {
        line::writeServeMarker(data, port);  // 服务还没开始应答时，上一次查询会把记号文件清掉
        url = line::runningService(data);
        return !url.empty();
    }));
    EXPECT_EQ(url, "http://127.0.0.1:" + std::to_string(port));

    // 能转发的：list、show、deploy、rollback。不能转发的返回 -1，由调用者拒绝
    EXPECT_EQ(line::forwardStationCommand(url, {"station", "list"}), 0);
    EXPECT_EQ(line::forwardStationCommand(url, {"station", "show", station}), 0);
    EXPECT_EQ(line::forwardStationCommand(url, {"station", "show", "nobody"}), 1);
    EXPECT_EQ(line::forwardStationCommand(url, {"station", "deploy", station, "nope"}), 1);
    EXPECT_EQ(line::forwardStationCommand(url, {"station", "bind", station, "folder", "x"}), -1);
    EXPECT_EQ(line::forwardStationCommand(url, {"station", "create", "新的"}), -1);
    const auto lenient = releaseAllowing(5);
    EXPECT_EQ(line::forwardStationCommand(url, {"station", "deploy", station, lenient}), 0);
    EXPECT_EQ(line->stations.getStation(station).value().pendingRelease, lenient);  // 真的落在了那个进程的工位上

    api.stop();
    apiThread.join();
    line::removeServeMarker(data);
}
