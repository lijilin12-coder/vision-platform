// vision-cli：命令行驱动适配器 + 组装根（composition root）。
// 组装根是唯一可以同时看到所有适配器和服务的地方，负责把它们连接起来。

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <random>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include "vision/catalog/api.h"
#include "vision/common/log.h"
#include "vision/common/path.h"
#include "vision/crash/crash_handler.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/logging/sinks.h"
#include "cli_app.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using namespace vision;
using namespace vision::cli;

namespace {

constexpr const char* kUsage = R"(用法：vision-cli [--data <目录>] <命令> [参数...]

算子
  operators                                   列出所有算子
方案编排（BC-2）
  recipe build <草稿描述.json>                 按描述文件创建方案（逐条调用编辑命令，规则照常检查）
  recipe new <名称>                            创建空方案
  recipe add-node <方案ID> <算子ID>
  recipe connect <方案ID> <n1.out> <n2.in>
  recipe set-param <方案ID> <节点> <参数> <值>
  recipe set-roi <方案ID> <节点> <x> <y> <宽> <高>
  recipe show <方案ID>
  recipe template add <方案ID> <名称> <样图> <x> <y> <宽> <高>
                                               从样图上框一块作为匹配模板，输出模板 ID；
                                               再用 recipe set-param <方案ID> <节点> <参数> <模板ID> 交给节点使用
  recipe template rename <方案ID> <模板ID> <新名称>
  recipe template remove <方案ID> <模板ID>     仍被节点使用的模板不能删除
  recipe list
  recipe debug <方案ID> <图像>                  调试运行草稿，显示每个节点的输出
  recipe publish <方案ID>                       发布，输出方案版本 ID
  version show <方案版本ID>                     以发布语言（JSON）显示方案版本
检测执行（BC-3）
  inspect <方案版本ID> <图像或目录>...          执行检测，输出判定、判定依据和各节点耗时
上线管理（BC-4）
  release create <方案版本ID>                   用方案版本创建上线包，输出上线包 ID
  release list
  release show <上线包ID>
  release withdraw <上线包ID>                   撤回：不允许再部署
工位运行（BC-5）
  station create <名称> [--auto-rollback N]     创建工位，输出工位 ID（N：连续多少次 ERROR 自动回滚，默认 3）
  station list
  station show <工位ID>
  station deploy <工位ID> <上线包ID>            部署：加载完成后，在下一次检测开始前生效
  station rollback <工位ID> [--withdraw]        回滚到上一个上线包；--withdraw 同时撤回出问题的上线包
  station run <工位ID> <图像或目录>... [--repeat N]
                                               热启动工位，对图像逐张检测
  station stress <工位ID> <上线包A> <上线包B> <图像或目录> [--count N] [--deploys N] [--rollbacks N]
                                               一边连续检测，一边在两个上线包之间反复部署和回滚，输出测量结果
方案助手（方案构建平台）
  workflow import <工作流.json>                 导入一个工作流（示例见 samples/workflows/）
  workflow list
  golden sample <输出目录> [--blind-lighting dim|same] [--scenario basic|glare|width|locate] [--workflow <工作流ID>]
                                               生成示例基准图集和一份任务书；dim（默认）表示盲测集的光照比调参集暗；
                                               glare：图上还有不算缺陷的大面积反光（配 bright-spot-glare 工作流）；
                                               width：测量零件的宽度（配 width-measure 工作流）；
                                               locate：零件位置和角度随机，先定位再测量（配 locate-width-measure 工作流，任务书里带匹配模板）；
                                               --workflow：任务书里改用另一个工作流，例如 bright-spot-normalized
  golden list | golden show <基准图集ID>
  assist run <任务书.json> (--advisor search | --script <顾问脚本.json> | --llm <大模型配置.json>)
                                               [--no-statistics] [--report <报告.md>]
                                               按工作流搭出候选方案并自动调参，输出助手报告。
                                               search：自己搜索参数的顾问；--script：照脚本出牌的顾问；
                                               --llm：大模型顾问（配置示例见 samples/assistant/llm.*.json，密钥放在环境变量里）
  assist list | assist show <会话ID>
  assist adopt <会话ID>                         采纳候选方案（只有人可以做这个决定）
  assist abandon <会话ID>
工具
  sample-images <输出目录> [宽 高]              生成示例图像（良品与带亮斑缺陷的产品），默认 640×480
  debug crash <segv|abort|throw>                故意让程序崩溃，用来验证崩溃报告和转储文件能否生成

选项
  --data <目录>          数据目录，默认 ./vision-data
  --log-level <级别>     写入日志文件的最低级别：trace / debug / info（默认）/ warn / error / off
  --events               在控制台显示领域事件
  --no-core-dump         崩溃时不生成 core 文件（macOS / Linux）

日志在 <数据目录>/logs/vision-cli.log，崩溃报告和转储文件在 <数据目录>/crashes/。
)";


// ---------- 故意崩溃：用来验证崩溃报告、转储文件、日志是否如预期留下 ----------
// 分成几层函数，是为了让调用栈里能看到一条完整的调用链。

#if defined(_MSC_VER)
#define VISION_NOINLINE __declspec(noinline)
#else
#define VISION_NOINLINE __attribute__((noinline))
#endif

VISION_NOINLINE void crashWriteThroughNull(volatile int* target) { *target = 42; }
VISION_NOINLINE void crashInOperator(const std::string& kind) {
    if (kind == "segv") {
        crashWriteThroughNull(nullptr);
    } else if (kind == "abort") {
        std::abort();
    } else {
        throw std::runtime_error("这是一个故意抛出、没有人捕获的异常");
    }
}
VISION_NOINLINE void crashInInspection(const std::string& kind) { crashInOperator(kind); }

int cmdDebugCrash(const std::string& kind) {
    if (kind != "segv" && kind != "abort" && kind != "throw") {
        std::cerr << "未知的崩溃类型：" << kind << "（可选 segv、abort、throw）\n";
        return 1;
    }
    log::warn("cli", "即将按要求故意崩溃", {{"kind", kind}});
    crashInInspection(kind);
    return 0;  // 不会执行到这里
}

ParamValue parseParam(const std::string& s) {
    if (s == "true") return true;
    if (s == "false") return false;
    try {
        size_t used = 0;
        double d = std::stod(s, &used);
        if (used == s.size()) return d;
    } catch (...) {
    }
    return std::string(s);
}

std::string paramText(const ParamValue& v) {
    if (std::holds_alternative<double>(v)) {
        std::ostringstream os;
        os << std::get<double>(v);
        return os.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
    return std::get<std::string>(v);
}

void printReport(const execution::InspectionReport& r, const std::string& source) {
    std::cout << "[" << r.verdict << "] " << source << "  (" << r.totalMs << " ms)\n";
    for (const auto& reason : r.reasons) {
        std::cout << "    " << (reason.pass ? "✓ " : "✗ ") << reason.nodeId << "：" << reason.text << "\n";
    }
    for (const auto& n : r.nodes) {
        std::cout << "      " << n.nodeId << " " << n.operatorId << " " << n.status << " " << n.durationMs << " ms";
        if (!n.message.empty()) std::cout << "  " << n.message;
        std::cout << "\n";
    }
}

int cmdOperators() {
    for (const auto& e : catalog::OperatorCatalog::global().registrationErrors()) {
        std::cerr << "注册失败：" << e.message << "\n";
    }
    for (const auto& m : catalog::OperatorCatalog::global().list()) {
        const auto& s = m.signature;
        std::cout << s.id << "@v" << s.majorVersion << "  " << m.displayName << "（" << m.category << "）\n";
        for (const auto& p : s.inputs) std::cout << "    输入 " << p.name << ": " << toString(p.type) << "\n";
        for (const auto& p : s.outputs) std::cout << "    输出 " << p.name << ": " << toString(p.type) << "\n";
        for (const auto& p : s.params)
            std::cout << "    参数 " << p.name << " = " << paramText(p.defaultValue) << "  " << p.label << "\n";
    }
    return 0;
}

// 草稿描述文件：{ "name": ..., "nodes": [{"key","operator","params","roi"}], "edges": [["a.out","b.in"]] }
int cmdRecipeBuild(App& app, const std::string& path) {
    std::ifstream in(pathFromUtf8(path));
    if (!in) return fail({"cli.read", "无法读取 " + path});
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        return fail({"cli.parse", e.what()});
    }
    auto id = app.authoring.createRecipe(j.at("name").get<std::string>());
    if (!id) return fail(id.error());
    const std::string rid = id.value();
    std::map<std::string, std::string> keyToNode;
    for (const auto& n : j.at("nodes")) {
        auto node = app.authoring.addNode(rid, n.at("operator").get<std::string>());
        if (!node) return fail(node.error());
        keyToNode[n.at("key").get<std::string>()] = node.value();
        if (n.contains("params")) {
            for (auto it = n["params"].begin(); it != n["params"].end(); ++it) {
                ParamValue v = it->is_boolean()  ? ParamValue(it->get<bool>())
                               : it->is_number() ? ParamValue(it->get<double>())
                                                 : ParamValue(it->get<std::string>());
                if (auto st = app.authoring.setParam(rid, node.value(), it.key(), v); !st) return fail(st.error());
            }
        }
        if (n.contains("roi")) {
            const auto& r = n["roi"];
            auto roi = Roi::rect(r.at("x"), r.at("y"), r.at("width"), r.at("height"));
            if (!roi) return fail(roi.error());
            if (auto st = app.authoring.setRoi(rid, node.value(), roi.value()); !st) return fail(st.error());
        }
    }
    auto mapRef = [&](const std::string& ref) {
        const auto dot = ref.find('.');
        auto it = keyToNode.find(ref.substr(0, dot));
        return it == keyToNode.end() ? ref : it->second + ref.substr(dot);
    };
    for (const auto& e : j.at("edges")) {
        if (auto st = app.authoring.connect(rid, mapRef(e.at(0)), mapRef(e.at(1))); !st) return fail(st.error());
    }
    std::cout << rid << "\n";
    return 0;
}

int cmdRecipeShow(App& app, const std::string& rid) {
    auto d = app.authoring.getDraft(rid);
    if (!d) return fail(d.error());
    std::cout << "方案 " << d.value().name << "（" << d.value().recipeId << "）\n";
    for (const auto& n : d.value().nodes) {
        std::cout << "  " << n.id << "  " << n.operatorId;
        for (const auto& [k, v] : n.params) std::cout << "  " << k << "=" << paramText(v);
        if (!n.roi.empty()) std::cout << "  roi=" << n.roi;
        std::cout << "\n";
    }
    for (const auto& e : d.value().edges) std::cout << "  " << e.from << " → " << e.to << "\n";
    for (const auto& t : d.value().templates) {
        std::cout << "  匹配模板 " << t.id << "  " << t.name << "  " << t.width << "×" << t.height << "  基准位置 ("
                  << t.referenceX << ", " << t.referenceY << ")  样图 " << t.sourceWidth << "×" << t.sourceHeight << "  "
                  << t.sha256.substr(0, 12) << "…\n";
    }
    return 0;
}

// recipe template add <方案ID> <名称> <样图> <x> <y> <宽> <高>：从样图上框一块作为匹配模板
int cmdTemplateAdd(App& app, const std::vector<std::string>& args) {
    auto source = imaging::loadImage(args[5]);
    if (!source) return fail(source.error());
    try {
        auto id = app.authoring.createMatchTemplate(args[3], args[4], source.value(), std::stoi(args[6]), std::stoi(args[7]),
                                                    std::stoi(args[8]), std::stoi(args[9]));
        if (!id) return fail(id.error());
        std::cout << id.value() << "\n";
        return 0;
    } catch (const std::exception&) {
        std::cerr << "x、y、宽、高必须是整数\n";
        return 1;
    }
}

int cmdInspect(App& app, const std::string& versionId, const std::vector<std::string>& inputs) {
    auto doc = app.authoring.getVersionDocument(versionId);
    if (!doc) return fail(doc.error());
    std::map<std::string, int> tally;
    for (const auto& path : expandImages(inputs)) {
        auto img = imaging::loadImage(pathToUtf8(path));
        if (!img) return fail(img.error());
        auto report = app.execution.inspect(doc.value(), img.value());
        if (!report) return fail(report.error());
        printReport(report.value(), pathToUtf8(path.filename()));
        ++tally[report.value().verdict];
    }
    std::cout << "合计：OK " << tally["OK"] << "，NG " << tally["NG"] << "，ERROR " << tally["ERROR"] << "\n";
    return 0;
}

int cmdDebug(App& app, const std::string& rid, const std::string& image) {
    auto doc = app.authoring.getDraftDocument(rid);
    if (!doc) return fail(doc.error());
    auto img = imaging::loadImage(image);
    if (!img) return fail(img.error());
    auto r = app.execution.debugRun(doc.value(), img.value());
    if (!r) return fail(r.error());
    printReport(r.value().report, image);
    for (const auto& n : r.value().report.nodes) {
        for (const auto& [port, text] : n.outputs) std::cout << "    " << n.nodeId << "." << port << " = " << text << "\n";
    }
    return 0;
}

// 示例图像：灰色背景上的“零件”（暗矩形），缺陷品带有亮斑。
int cmdSampleImages(const std::string& dir, int width, int height) {
    const fs::path outDir = pathFromUtf8(dir);
    fs::create_directories(outDir);
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 4);
    const double sx = width / 640.0, sy = height / 480.0, sr = std::min(sx, sy);
    const auto px = [sx](double v) { return static_cast<int>(std::lround(v * sx)); };
    const auto py = [sy](double v) { return static_cast<int>(std::lround(v * sy)); };
    auto make = [&](int defects) {
        cv::Mat m(height, width, CV_8UC3, cv::Scalar(150, 150, 150));
        cv::rectangle(m, cv::Rect(px(120), py(90), px(400), py(300)), cv::Scalar(70, 75, 80), cv::FILLED);
        for (int i = 0; i < defects; ++i) {
            cv::circle(m, cv::Point(px(170 + 90 * i), py(160 + 45 * (i % 3))), static_cast<int>(std::lround((6 + 2 * i) * sr)),
                       cv::Scalar(235, 235, 235), cv::FILLED);
        }
        for (int y = 0; y < m.rows; ++y) {
            for (int x = 0; x < m.cols; ++x) {
                auto& pixel = m.at<cv::Vec3b>(y, x);
                const double n = noise(rng);
                for (int c = 0; c < 3; ++c) pixel[c] = cv::saturate_cast<uchar>(pixel[c] + n);
            }
        }
        return m;
    };
    struct Sample { const char* name; int defects; };
    for (auto s : {Sample{"ok_01.png", 0}, Sample{"ok_02.png", 0}, Sample{"ng_01_one_spot.png", 1},
                   Sample{"ng_02_three_spots.png", 3}}) {
        auto img = imaging::fromMat(make(s.defects));
        if (!img) return fail(img.error());
        const std::string file = pathToUtf8(outDir / s.name);
        if (auto st = imaging::saveImage(file, img.value()); !st) return fail(st.error());
        std::cout << file << "\n";
    }
    return 0;
}


// 取得 UTF-8 编码的命令行参数，并让控制台按 UTF-8 显示。
// Windows 上 main 的 argv 是本地编码（中文系统为 GBK），控制台默认也不是 UTF-8，
// 不处理的话，中文参数会变成乱码，中文输出也会显示成乱码。
std::vector<std::string> utf8Args(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::vector<std::string> args;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) return args;
    for (int i = 1; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(size > 0 ? static_cast<size_t>(size - 1) : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), size, nullptr, nullptr);
        args.push_back(std::move(arg));
    }
    LocalFree(wide);
    return args;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

}  // namespace

namespace vision::cli {

int fail(const Error& e) {
    log::error("cli", "命令失败", {{"code", e.code}, {"message", e.message}});
    std::cerr << "错误：" << e.message << "  (" << e.code << ")\n";
    return 1;
}

std::vector<fs::path> expandImages(const std::vector<std::string>& args) {
    std::vector<fs::path> out;
    for (const auto& a : args) {
        const fs::path p = pathFromUtf8(a);
        if (fs::is_directory(p)) {
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(p)) {
                const auto ext = pathToUtf8(e.path().extension());
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") files.push_back(e.path());
            }
            std::sort(files.begin(), files.end());
            out.insert(out.end(), files.begin(), files.end());
        } else {
            out.push_back(p);
        }
    }
    return out;
}

}  // namespace vision::cli

int main(int argc, char** argv) {
    std::vector<std::string> args = utf8Args(argc, argv);
    fs::path data = "vision-data";
    bool verbose = false;
    bool coreDumps = true;
    LogLevel logLevel = LogLevel::Info;
    for (size_t i = 0; i < args.size();) {
        if (args[i] == "--data" && i + 1 < args.size()) {
            data = pathFromUtf8(args[i + 1]);
            args.erase(args.begin() + i, args.begin() + i + 2);
        } else if (args[i] == "--log-level" && i + 1 < args.size()) {
            logLevel = parseLogLevel(args[i + 1], LogLevel::Info);
            args.erase(args.begin() + i, args.begin() + i + 2);
        } else if (args[i] == "--no-core-dump") {
            coreDumps = false;
            args.erase(args.begin() + i);
        } else if (args[i] == "--events") {
            verbose = true;
            args.erase(args.begin() + i);
        } else {
            ++i;
        }
    }
    if (args.empty() || args[0] == "help" || args[0] == "--help") {
        std::cout << kUsage;
        return args.empty() ? 1 : 0;
    }

    // 日志和崩溃处理要最先准备好：之后任何地方出问题，都能留下记录。
    logging::LoggingOptions logOptions;
    logOptions.dir = data / "logs";
    logOptions.name = "vision-cli";
    logOptions.level = logLevel;
    logOptions.consoleLevel = LogLevel::Off;  // 命令行程序自己负责向用户输出，日志只写文件
    logging::setupLogging(logOptions);

    crash::CrashHandlerOptions crashOptions;
    crashOptions.dir = data / "crashes";
    crashOptions.appName = "vision-cli";
    crashOptions.enableCoreDumps = coreDumps;
    crash::installCrashHandler(crashOptions);

    std::string commandLine;
    for (const auto& a : args) commandLine += (commandLine.empty() ? "" : " ") + a;
    log::info("cli", "启动", {{"command", commandLine}});

    App app(data);
    app.verboseEvents = verbose;
    const auto& cmd = args[0];
    auto need = [&](size_t n) {
        if (args.size() < n) {
            std::cerr << "参数不足。\n" << kUsage;
            return false;
        }
        return true;
    };

    if (cmd == "operators") return cmdOperators();
    if (cmd == "debug" && need(3) && args[1] == "crash") return cmdDebugCrash(args[2]);
    if (cmd == "sample-images" && need(2)) {
        int width = 640, height = 480;
        if (args.size() >= 4) {
            width = std::atoi(args[2].c_str());
            height = std::atoi(args[3].c_str());
            if (width < 160 || height < 120) {
                std::cerr << "图像尺寸至少为 160×120\n";
                return 1;
            }
        }
        return cmdSampleImages(args[1], width, height);
    }
    if (cmd == "release" && need(2)) return runReleaseCommand(app, args);
    if (cmd == "station" && need(2)) return runStationCommand(app, args);
    if (cmd == "golden" && need(2)) return runGoldenCommand(app, args);
    if (cmd == "workflow" && need(2)) return runWorkflowCommand(app, args);
    if (cmd == "assist" && need(2)) return runAssistCommand(app, args);
    if (cmd == "inspect" && need(3)) return cmdInspect(app, args[1], {args.begin() + 2, args.end()});
    if (cmd == "version" && need(3) && args[1] == "show") {
        auto doc = app.authoring.getVersionDocument(args[2]);
        if (!doc) return fail(doc.error());
        std::cout << persistence::toJson(doc.value()) << "\n";
        return 0;
    }
    if (cmd == "recipe" && need(2)) {
        const auto& sub = args[1];
        if (sub == "list") {
            for (const auto& id : app.authoring.listRecipes()) std::cout << id << "\n";
            return 0;
        }
        if (sub == "build" && need(3)) return cmdRecipeBuild(app, args[2]);
        if (sub == "new" && need(3)) {
            auto id = app.authoring.createRecipe(args[2]);
            if (!id) return fail(id.error());
            std::cout << id.value() << "\n";
            return 0;
        }
        if (sub == "add-node" && need(4)) {
            auto n = app.authoring.addNode(args[2], args[3]);
            if (!n) return fail(n.error());
            std::cout << n.value() << "\n";
            return 0;
        }
        if (sub == "connect" && need(5)) {
            auto st = app.authoring.connect(args[2], args[3], args[4]);
            return st ? 0 : fail(st.error());
        }
        if (sub == "set-param" && need(6)) {
            auto st = app.authoring.setParam(args[2], args[3], args[4], parseParam(args[5]));
            return st ? 0 : fail(st.error());
        }
        if (sub == "set-roi" && need(8)) {
            auto roi = Roi::rect(std::stod(args[4]), std::stod(args[5]), std::stod(args[6]), std::stod(args[7]));
            if (!roi) return fail(roi.error());
            auto st = app.authoring.setRoi(args[2], args[3], roi.value());
            return st ? 0 : fail(st.error());
        }
        if (sub == "show" && need(3)) return cmdRecipeShow(app, args[2]);
        if (sub == "template" && need(3)) {
            const auto& action = args[2];
            if (action == "add" && need(10)) return cmdTemplateAdd(app, args);
            if (action == "remove" && need(5)) {
                auto st = app.authoring.removeMatchTemplate(args[3], args[4]);
                return st ? 0 : fail(st.error());
            }
            if (action == "rename" && need(6)) {
                auto st = app.authoring.renameMatchTemplate(args[3], args[4], args[5]);
                return st ? 0 : fail(st.error());
            }
        }
        if (sub == "debug" && need(4)) return cmdDebug(app, args[2], args[3]);
        if (sub == "publish" && need(3)) {
            auto v = app.authoring.publish(args[2]);
            if (!v) return fail(v.error());
            std::cout << v.value() << "\n";
            return 0;
        }
    }
    std::cerr << "未知命令。\n" << kUsage;
    return 1;
}
