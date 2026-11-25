// vision-runtime：把平台包成一个一直开着的本地服务，通过 HTTP 接口使用（接口见 docs/dev/http-api.md）。
//
// 和 vision-cli 的关系：同一份组装好的平台（apps/common/platform.h），换了一种使用方式。
// 命令行是“启动、做一件事、退出”；服务是一直开着，同时应付多个请求。

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "http_api.h"
#include "vision/common/log.h"
#include "vision/common/path.h"
#include "vision/crash/crash_handler.h"
#include "vision/logging/sinks.h"

#include "utf8_args.h"

using namespace vision;

namespace {

constexpr const char* kUsage = R"(用法：vision-runtime [选项]

  --data <目录>          数据目录，默认 ./vision-data（和 vision-cli 用同一个目录，就能互相看到对方的方案）
  --port <端口>          监听的端口，默认 8750
  --host <地址>          监听的地址，默认 127.0.0.1（只接受本机的连接）。
                         这个服务没有登录和权限：不要让它监听不可信的网络
  --allow-unaccepted     调试用：允许部署还没有验收的上线包（验收没通过的仍然不行）。不要在正式生产中使用
  --training-url <地址>  训练服务的地址（也可以用环境变量 VISION_TRAINING_URL）
  --web <目录>           界面的文件在哪里，默认 ./web/dist（编译界面：cd web && npm install && npm run build）。
                         目录存在的话，浏览器打开 http://127.0.0.1:8750/ 就是界面

启动之后：浏览器或 curl 访问 http://127.0.0.1:8750/api/health
按 Ctrl+C 停止。日志在 <数据目录>/logs/vision-runtime.log。
)";

std::atomic<bool> stopRequested{false};
void onSignal(int) { stopRequested = true; }  // 信号处理函数里只能做这么一点事

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);  // Windows 上数据目录可能带中文
    std::string dataDir = "vision-data", host = "127.0.0.1", trainingUrl, webDir = "web/dist";
    int port = 8750;
    bool allowUnaccepted = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool hasValue = i + 1 < args.size();
        if (arg == "--help" || arg == "help") {
            std::cout << kUsage;
            return 0;
        } else if (arg == "--data" && hasValue) {
            dataDir = args[++i];
        } else if (arg == "--web" && hasValue) {
            webDir = args[++i];
        } else if (arg == "--host" && hasValue) {
            host = args[++i];
        } else if (arg == "--allow-unaccepted") {
            allowUnaccepted = true;
        } else if (arg == "--training-url" && hasValue) {
            trainingUrl = args[++i];
        } else if (arg == "--port" && hasValue) {
            port = std::atoi(args[++i].c_str());
            if (port < 1 || port > 65535) {
                std::cerr << "端口必须在 1～65535 之间\n";
                return 1;
            }
        } else {
            std::cerr << "看不懂的参数：" << arg << "\n" << kUsage;
            return 1;
        }
    }
    if (trainingUrl.empty()) {
        if (const char* env = std::getenv("VISION_TRAINING_URL")) trainingUrl = env;
    }
    const std::filesystem::path data = pathFromUtf8(dataDir);

    logging::LoggingOptions logOptions;
    logOptions.dir = data / "logs";
    logOptions.name = "vision-runtime";
    logOptions.consoleLevel = LogLevel::Off;
    logging::setupLogging(logOptions);
    crash::CrashHandlerOptions crashOptions;
    crashOptions.dir = data / "crashes";
    crashOptions.appName = "vision-runtime";
    crash::installCrashHandler(crashOptions);

    platform::Platform platform(data, trainingUrl);
    if (allowUnaccepted) {
        platform.stations.allowUnacceptedReleases(true);
        log::warn("runtime", "调试选项 --allow-unaccepted：允许部署还没有验收的上线包。不要在正式生产中使用");
    }
    runtime::ApiServer server(platform);
    if (server.bind(host, port) < 0) {
        std::cerr << "无法监听 " << host << ":" << port << "（端口是不是被别的程序占用了？）\n";
        log::error("runtime", "无法监听", {{"host", host}, {"port", std::to_string(port)}});
        return 1;
    }
    if (host != "127.0.0.1" && host != "localhost") {
        std::cerr << "注意：正在监听 " << host << "。这个服务没有登录和权限，能连到这个地址的人都可以操作它。\n";
    }
    std::error_code webEc;
    const bool hasWeb = std::filesystem::is_regular_file(pathFromUtf8(webDir) / "index.html", webEc) && server.serveWebFiles(webDir);
    log::info("runtime", "启动", {{"host", host}, {"port", std::to_string(port)}, {"data", dataDir}});
    std::cout << "vision-runtime 已启动。数据目录 " << dataDir << "。按 Ctrl+C 停止。\n";
    if (hasWeb) {
        std::cout << "界面：  http://" << host << ":" << port << "/\n";
    } else {
        std::cout << "界面：  没有找到（" << webDir << "/index.html 不存在）。先编译：cd web && npm install && npm run build\n";
    }
    std::cout << "接口：  http://" << host << ":" << port << "/api/health" << std::endl;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    // 处理请求在另一个线程里；主线程只等“该停了”的信号
    std::thread serving([&server] { server.serve(); });
    while (!stopRequested) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "正在停止……" << std::endl;
    server.stop();
    serving.join();
    log::info("runtime", "停止");
    return 0;
}
