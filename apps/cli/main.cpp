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
#include <sstream>
#include <string>
#include <vector>

#include "vision/authoring/api.h"
#include "vision/catalog/api.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/execution/api.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/persistence/json_repositories.h"

namespace fs = std::filesystem;
using namespace vision;

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
  recipe list
  recipe debug <方案ID> <图像>                  调试运行草稿，显示每个节点的输出
  recipe publish <方案ID>                       发布，输出方案版本 ID
  version show <方案版本ID>                     以发布语言（JSON）显示方案版本
检测执行（BC-3）
  inspect <方案版本ID> <图像或目录>...          执行检测，输出判定、判定依据和各节点耗时
工具
  sample-images <输出目录>                      生成示例图像（良品与带亮斑缺陷的产品）

默认数据目录：./vision-data
)";

struct App {
    eventbus::InMemoryEventBus bus;
    persistence::JsonRecipeRepository recipes;
    persistence::JsonRecipeVersionRepository versions;
    authoring::AuthoringService authoring;
    execution::ExecutionService execution;
    bool verboseEvents = false;

    explicit App(const fs::path& data)
        : recipes(data),
          versions(data),
          authoring(recipes, versions, catalog::OperatorCatalog::global(), bus),
          execution(catalog::OperatorCatalog::global(), bus) {
        bus.subscribe("*", [this](const EventPtr& e) {
            if (verboseEvents) std::cerr << "  [事件] " << e->name() << "\n";
        });
    }
};

int fail(const Error& e) {
    std::cerr << "错误：" << e.message << "  (" << e.code << ")\n";
    return 1;
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
    std::ifstream in(path);
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
    return 0;
}

std::vector<fs::path> expandImages(const std::vector<std::string>& args) {
    std::vector<fs::path> out;
    for (const auto& a : args) {
        if (fs::is_directory(a)) {
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(a)) {
                const auto ext = e.path().extension().string();
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") files.push_back(e.path());
            }
            std::sort(files.begin(), files.end());
            out.insert(out.end(), files.begin(), files.end());
        } else {
            out.emplace_back(a);
        }
    }
    return out;
}

int cmdInspect(App& app, const std::string& versionId, const std::vector<std::string>& inputs) {
    auto doc = app.authoring.getVersionDocument(versionId);
    if (!doc) return fail(doc.error());
    std::map<std::string, int> tally;
    for (const auto& path : expandImages(inputs)) {
        auto img = imaging::loadImage(path.string());
        if (!img) return fail(img.error());
        auto report = app.execution.inspect(doc.value(), img.value());
        if (!report) return fail(report.error());
        printReport(report.value(), path.filename().string());
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
int cmdSampleImages(const std::string& dir) {
    fs::create_directories(dir);
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0, 4);
    auto make = [&](int defects) {
        cv::Mat m(480, 640, CV_8UC3, cv::Scalar(150, 150, 150));
        cv::rectangle(m, cv::Rect(120, 90, 400, 300), cv::Scalar(70, 75, 80), cv::FILLED);
        for (int i = 0; i < defects; ++i) {
            cv::circle(m, cv::Point(170 + 90 * i, 160 + 45 * (i % 3)), 6 + 2 * i, cv::Scalar(235, 235, 235), cv::FILLED);
        }
        for (int y = 0; y < m.rows; ++y) {
            for (int x = 0; x < m.cols; ++x) {
                auto& px = m.at<cv::Vec3b>(y, x);
                const double n = noise(rng);
                for (int c = 0; c < 3; ++c) px[c] = cv::saturate_cast<uchar>(px[c] + n);
            }
        }
        return m;
    };
    struct Sample { const char* name; int defects; };
    for (auto s : {Sample{"ok_01.png", 0}, Sample{"ok_02.png", 0}, Sample{"ng_01_one_spot.png", 1},
                   Sample{"ng_02_three_spots.png", 3}}) {
        auto img = imaging::fromMat(make(s.defects));
        if (!img) return fail(img.error());
        if (auto st = imaging::saveImage((fs::path(dir) / s.name).string(), img.value()); !st) return fail(st.error());
        std::cout << (fs::path(dir) / s.name).string() << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path data = "vision-data";
    bool verbose = false;
    for (size_t i = 0; i < args.size();) {
        if (args[i] == "--data" && i + 1 < args.size()) {
            data = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
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
    if (cmd == "sample-images" && need(2)) return cmdSampleImages(args[1]);
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
