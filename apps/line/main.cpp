// vision-line：产线运行平台。
//
// 它能做的事比 vision-cli 少得多，这是故意的：产线上的电脑只运行别人搭好、验证过的方案。
//   有：导入方案包、上线包、工位（部署、热切换、回滚、绑定图像源）、让工位持续运行
//   没有：方案编排、调试运行、方案助手、数据集、训练
// 方案和模型只有一个来路：方案包（package import）。
//
// 组装见 apps/common/line_platform.h；两个平台的划分见 docs/ddd/13-two-programs.md。

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <thread>

#include "line_api.h"
#include "line_commands.h"
#include "utf8_args.h"
#include "vision/common/log.h"
#include "vision/common/path.h"
#include "vision/crash/crash_handler.h"
#include "vision/logging/sinks.h"

using namespace vision;

namespace {

constexpr const char* kUsage = R"(用法：vision-line [--data <目录>] <命令> [参数...]

产线运行平台：只运行别处搭好的方案。方案和模型只能通过方案包带进来。

方案包
  package import <文件.vpkg>                    导入方案包（方案版本 + 它用到的模板和模型）
  package show <文件.vpkg>                      只看包里有什么
上线包
  release import <文件.vrel>                    导入上线包文件：方案版本、模板、模型，和一个验收通过的上线包。之后直接部署
  release create <方案版本ID>                   用导入的方案版本创建上线包（没有验收记录，只有加了 --allow-unaccepted 才能部署）
  release list | release show <上线包ID> | release withdraw <上线包ID>
工位
  station create <名称> [--auto-rollback N]     创建工位，输出工位 ID
  station list | station show <工位ID>
  station bind <工位ID> folder <目录>           绑定图像源（只能在工位停止时做）
  station unbind <工位ID>
  station deploy <工位ID> <上线包ID>            部署：加载完成后，在下一次检测开始前生效
  station rollback <工位ID> [--withdraw]
  station run <工位ID> [--follow 秒]            让一个工位把图像源里现有的图像检测完
  station records <工位ID> [--ng] [--unreviewed | --misjudged] [--limit N]
                                               看保存下来的检测记录（从新到旧）。每行开头是记录的编号。
                                               --ng 只看不合格的；--unreviewed 还没复判的；--misjudged 复判确认判错了的
  station review <工位ID> <记录编号> ok|ng [--note 文字] [--by 谁]
                                               复判：人确认这一次实际是合格（ok）还是不合格（ng）。可以改，以最近一次为准
  station export-reviewed <工位ID> <目录> [--misjudged]
                                               把复判过的图像导出成 <目录>/ok 和 <目录>/ng（可以直接拿去建基准图集、导入样本）
  station summary <工位ID>                      检测了多少、良率、每种缺陷出现了多少个
运行
  serve [<工位ID>...] [--port N] [--exit-when-idle 秒]
                                               让工位持续运行：每个工位从自己的图像源取图、检测，直到按 Ctrl+C。
                                               不写工位 ID 就是所有绑定了图像源的工位。
                                               运行期间，在另一个终端里照常执行 station deploy / rollback / show / list：
                                               它们会自动交给正在运行的服务去做，热切换照常生效。
                                               （服务在本机的一个端口上听这些请求，默认 8760，--port 可以改）
模型（只能看）
  model list | model show <模型版本ID>

选项
  --data <目录>          数据目录，默认 ./vision-line-data
  --events               在控制台显示领域事件
  --keep-images <哪些>   检测时留下哪些图像：
                           all（默认）不合格的和出错的长期保留；判 OK 的留一段时间，过期自动清掉
                           ng   只留不合格的和出错的。判 OK 的不留——事后发现漏检时没有图像可看
                           none 都不留
  --image-retention-hours <小时>
                         判 OK 的图像留多久，默认 72。复判过的图像会转成长期保留，不受它影响
  --allow-unaccepted     调试用：允许部署还没有验收的上线包（验收没通过的仍然不行）。不要在正式生产中使用

日志在 <数据目录>/logs/vision-line.log，崩溃报告在 <数据目录>/crashes/。
)";

std::atomic<bool> stopRequested{false};
void onSignal(int) { stopRequested = true; }

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args = utf8Args(argc, argv);
    std::string dataDir = "vision-line-data";
    bool verbose = false;
    bool allowUnaccepted = false;
    std::string keepImages, retentionHours;
    int exitWhenIdle = 0, port = 8760;
    for (std::size_t i = 0; i < args.size();) {
        if (args[i] == "--data" && i + 1 < args.size()) {
            dataDir = args[i + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        } else if (args[i] == "--exit-when-idle" && i + 1 < args.size()) {
            exitWhenIdle = std::atoi(args[i + 1].c_str());
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        } else if (args[i] == "--port" && i + 1 < args.size()) {
            port = std::atoi(args[i + 1].c_str());
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        } else if (args[i] == "--events") {
            verbose = true;
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (args[i] == "--keep-images" && i + 1 < args.size()) {
            keepImages = args[i + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        } else if (args[i] == "--image-retention-hours" && i + 1 < args.size()) {
            retentionHours = args[i + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
        } else if (args[i] == "--allow-unaccepted") {
            allowUnaccepted = true;
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (args.empty() || args[0] == "help" || args[0] == "--help") {
        std::cout << kUsage;
        return args.empty() ? 1 : 0;
    }
    const std::filesystem::path data = pathFromUtf8(dataDir);

    logging::LoggingOptions logOptions;
    logOptions.dir = data / "logs";
    logOptions.name = "vision-line";
    logOptions.consoleLevel = LogLevel::Off;
    logging::setupLogging(logOptions);
    crash::CrashHandlerOptions crashOptions;
    crashOptions.dir = data / "crashes";
    crashOptions.appName = "vision-line";
    crash::installCrashHandler(crashOptions);

    std::string commandLine;
    for (const auto& a : args) commandLine += (commandLine.empty() ? "" : " ") + a;
    log::info("line", "启动", {{"command", commandLine}});

    platform::LinePlatform app(data);
    app.verboseEvents = verbose;
    if (auto st = platform::applyImageOptions(app, keepImages, retentionHours); !st) {
        std::cerr << st.error().message << "\n";
        return 1;
    }
    (void)app.quality.cleanExpiredImages();  // 启动时清一次过期的检测图像
    if (allowUnaccepted) {
        app.stations.allowUnacceptedReleases(true);
        log::warn("line", "调试选项 --allow-unaccepted：允许部署还没有验收的上线包。不要在正式生产中使用");
    }
    const std::string& cmd = args[0];

    if (cmd == "package" && args.size() >= 3) {
        if (args[1] == "export") {
            std::cerr << "产线运行平台不导出方案包：方案包是在方案构建平台上做的\n";
            return 1;
        }
        return cli::runPackageCommand(app, args);
    }
    if (cmd == "release" && args.size() >= 2) {
        if (args[1] == "export") {
            std::cerr << "产线运行平台不导出上线包文件：上线包文件是在方案构建平台上做的\n";
            return 1;
        }
        return cli::runReleaseCommand(app, args);
    }
    if (cmd == "station" && args.size() >= 2) {
        if (args[1] == "stress") {
            std::cerr << "station stress 是方案构建平台上的测量工具，产线运行平台没有\n";
            return 1;
        }
        // 服务正在运行的话，工位的状态在它的内存里：对工位的操作要交给它，不能在这个进程里另做一份
        // 检测记录在磁盘上，谁都可以读：服务运行着的时候也能直接看
        if (args[1] == "records" || args[1] == "summary" || args[1] == "review" || args[1] == "export-reviewed") {
            return cli::runStationCommand(app, args);
        }
        if (const std::string service = line::runningService(data); !service.empty()) {
            const int code = line::forwardStationCommand(service, args);
            if (code >= 0) return code;
            std::cerr << "服务正在运行（vision-line serve）。运行期间只能 station list / show / deploy / rollback；\n"
                         "其他对工位的操作（创建、绑定图像源、run）请先停掉服务\n";
            return 1;
        }
        return cli::runStationCommand(app, args);
    }
    if (cmd == "model") return cli::runModelViewCommand(app, args);
    if (cmd == "serve") {
        if (!line::runningService(data).empty()) {
            std::cerr << "这个数据目录上已经有一个服务在运行了\n";
            return 1;
        }
        app.imageJanitor.start();  // 服务长时间运行：每小时清一次过期的检测图像
        line::ControlApi api(app);
        const int bound = api.bind("127.0.0.1", port);
        if (bound < 0) {
            std::cerr << "无法监听 127.0.0.1:" << port << "（端口是不是被别的程序占用了？可以用 --port 换一个）\n";
            return 1;
        }
        std::thread serving([&api] { api.serve(); });
        line::writeServeMarker(data, bound);
        std::cout << "控制接口：http://127.0.0.1:" << bound << "（只接受本机的连接）。按 Ctrl+C 停止。" << std::endl;
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        const int code = cli::serveStations(app, {args.begin() + 1, args.end()}, exitWhenIdle, stopRequested);
        line::removeServeMarker(data);
        api.stop();
        serving.join();
        return code;
    }
    std::cerr << "产线运行平台没有这个命令：" << cmd << "\n";
    if (cmd == "recipe" || cmd == "assist" || cmd == "train" || cmd == "sample" || cmd == "dataset" || cmd == "golden" || cmd == "workflow") {
        std::cerr << "（搭方案、调试、训练在方案构建平台上做：vision-cli。做好之后用 package export 导出，再到这里 package import）\n";
    }
    return 1;
}
