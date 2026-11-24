// 产线运行平台的命令：release …、station …、package …、model（只看）、serve（让工位持续运行）。
// vision-cli 和 vision-line 共用。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <memory>
#include <sstream>
#include <thread>

#include "line_commands.h"
#include "station_runner.h"
#include "vision/common/log.h"
#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <psapi.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace vision::cli {

namespace {

using Clock = std::chrono::steady_clock;

double millisSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// 取出 “--名字 数值” 形式的选项并把它从参数里去掉；没有给出时返回默认值。
int takeIntOption(std::vector<std::string>& args, const std::string& name, int fallback) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            const int value = std::atoi(args[i + 1].c_str());
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            return value;
        }
    }
    return fallback;
}

bool takeFlag(std::vector<std::string>& args, const std::string& name) {
    const auto it = std::find(args.begin(), args.end(), name);
    if (it == args.end()) return false;
    args.erase(it);
    return true;
}

int usageError(const char* text) {
    std::cerr << "参数不对。用法：" << text << "\n";
    return 1;
}

// 上线包、工位的 ID 很长，显示时只取前 8 位
std::string shortId(const std::string& id) { return id.size() > 8 ? id.substr(0, 8) : id; }

void printStation(const station::StationView& v) {
    std::cout << "工位 " << v.name << "  (" << v.id << ")\n"
              << "  状态          " << v.state << "\n"
              << "  图像源        " << (v.imageSourceKind.empty() ? "（没有绑定）" : v.imageSourceKind + "：" + v.imageSourceLocation) << "\n"
              << "  生效的上线包  " << (v.activeRelease.empty() ? "（无）" : v.activeRelease) << "\n"
              << "  待切换        "
              << (v.pendingRelease.empty() ? "（无）" : v.pendingRelease + (v.pendingReady ? "（已就绪）" : "（加载中）")) << "\n"
              << "  可回滚到      ";
    if (v.releaseHistory.empty()) std::cout << "（无）";
    for (auto it = v.releaseHistory.rbegin(); it != v.releaseHistory.rend(); ++it) std::cout << shortId(*it) << " ";
    std::cout << "\n  已检测        " << v.sequence << " 次；连续 ERROR " << v.consecutiveErrors << " 次\n";
}

// ---------- 测量用的小工具 ----------

struct Memory {
    double currentMb = 0;  // 现在占用的物理内存
    double peakMb = 0;     // 进程启动以来的最高值
};

// 读取本进程占用的物理内存。读不到时返回 0。
Memory processMemory() {
    Memory m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        m.currentMb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
        m.peakMb = static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
    }
#elif defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        m.peakMb = static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);  // macOS 上单位是字节
        m.currentMb = m.peakMb;                                               // 这里只取得到峰值
    }
#else
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream in(line);
        std::string key;
        double kb = 0;
        in >> key >> kb;
        if (key == "VmRSS:") m.currentMb = kb / 1024.0;
        if (key == "VmHWM:") m.peakMb = kb / 1024.0;
    }
#endif
    return m;
}

struct Distribution {
    double median = 0, p99 = 0, max = 0;
    std::size_t count = 0;
};

Distribution summarize(std::vector<double> values) {
    Distribution d;
    d.count = values.size();
    if (values.empty()) return d;
    std::sort(values.begin(), values.end());
    d.median = values[values.size() / 2];
    d.p99 = values[std::min(values.size() - 1, values.size() * 99 / 100)];
    d.max = values.back();
    return d;
}

void printDistribution(const char* name, const Distribution& d, const char* unit) {
    std::cout << "  " << name << "：";
    if (d.count == 0) {
        std::cout << "（没有数据）\n";
        return;
    }
    std::cout << std::fixed << std::setprecision(3) << "中位数 " << d.median << "，99% 不超过 " << d.p99 << "，最大 " << d.max << " " << unit
              << "（" << d.count << " 个样本）\n";
}

Result<std::vector<Image>> loadImages(const std::vector<std::string>& inputs, std::vector<std::string>* names = nullptr) {
    std::vector<Image> images;
    for (const auto& path : expandImages(inputs)) {
        auto img = imaging::loadImage(pathToUtf8(path));
        if (!img) return img.error();
        images.push_back(std::move(img).value());
        if (names != nullptr) names->push_back(pathToUtf8(path.filename()));
    }
    if (images.empty()) return Result<std::vector<Image>>::failure("cli.no_images", "没有找到图像文件");
    return images;
}

// 让工位进入运行状态。还没有任何上线包时给出明确的提示。
Status startStation(platform::LinePlatform& app, const std::string& stationId) {
    if (auto st = app.stations.waitForLoading(stationId); !st) return st;
    return app.stations.start(stationId);
}

// ---------- station run ----------

int cmdStationRun(platform::LinePlatform& app, std::vector<std::string> args) {
    const int repeat = std::max(1, takeIntOption(args, "--repeat", 1));
    // --follow N：图像源暂时没有新图像时不退出，再等最多 N 秒（一边运行一边往目录里放图像时用）
    const int followSeconds = std::max(0, takeIntOption(args, "--follow", 0));
    if (args.size() < 3) return usageError("station run <工位ID> [<图像或目录>...] [--repeat N] [--follow 秒]");
    const std::string stationId = args[2];
    const auto print = [](const station::StationInspection& v, const std::string& name) {
        if (v.swapped) std::cout << "  —— 已切换到上线包 " << v.releaseId << " ——\n";
        std::cout << "#" << v.sequence << " [" << v.report.verdict << "] " << name << "  (上线包 " << shortId(v.releaseId) << "，" << std::fixed
                  << std::setprecision(2) << v.report.totalMs << " ms)\n";
        for (const auto& reason : v.report.reasons) {
            std::cout << "    " << (reason.pass ? "✓ " : "✗ ") << reason.nodeId << "：" << reason.text << "\n";
        }
        printResultItems(v.report.result);
    };

    if (args.size() == 3) {
        // 没有给图像：从工位绑定的图像源取（STA-01）
        if (auto st = startStation(app, stationId); !st) return fail(st.error());
        std::map<std::string, int> tally;
        int unreadable = 0;
        auto idleSince = std::chrono::steady_clock::now();
        for (;;) {
            auto r = app.stations.inspectNext(stationId);
            if (!r) {
                // 一帧图像读不出来：记一笔，接着取后面的。别的错误（工位没有图像源、被停掉了）就此结束
                if (r.error().code != "image_source.bad_frame") {
                    (void)app.stations.stop(stationId);
                    return fail(r.error());
                }
                std::cout << "  读不出来，跳过：" << r.error().message << "\n";
                ++unreadable;
                continue;
            }
            if (!r.value()) {
                const auto idle = std::chrono::steady_clock::now() - idleSince;
                if (idle >= std::chrono::seconds(followSeconds)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            idleSince = std::chrono::steady_clock::now();
            print(*r.value(), r.value()->frameName);
            ++tally[r.value()->report.verdict];
        }
        (void)app.stations.stop(stationId);
        std::cout << "合计：OK " << tally["OK"] << "，NG " << tally["NG"] << "，ERROR " << tally["ERROR"];
        if (unreadable > 0) std::cout << "；读不出来的图像 " << unreadable << " 幅";
        std::cout << "\n";
        return 0;
    }

    std::vector<std::string> names;
    auto images = loadImages({args.begin() + 3, args.end()}, &names);
    if (!images) return fail(images.error());
    if (auto st = startStation(app, stationId); !st) return fail(st.error());

    std::map<std::string, int> tally;
    for (int round = 0; round < repeat; ++round) {
        for (std::size_t i = 0; i < images.value().size(); ++i) {
            auto r = app.stations.inspect(stationId, images.value()[i]);
            if (!r) return fail(r.error());
            const auto& v = r.value();
            print(v, names[i]);
            ++tally[v.report.verdict];
        }
    }
    (void)app.stations.stop(stationId);
    std::cout << "合计：OK " << tally["OK"] << "，NG " << tally["NG"] << "，ERROR " << tally["ERROR"] << "\n";
    return 0;
}

// ---------- station stress ----------
// 一边连续检测，一边由另一个线程在两个上线包之间反复部署、回滚。
// 检查：没有检测丢失；检测序号连续；每次检测用的方案版本，就是它开始时拿到的那个上线包里的方案版本。
// 测量：单次检测耗时、热切换本身的耗时、上线包的加载耗时、写一条日志的耗时、内存占用。

int cmdStationStress(platform::LinePlatform& app, std::vector<std::string> args) {
    const int total = std::max(100, takeIntOption(args, "--count", 10000));
    const int deployments = std::max(1, takeIntOption(args, "--deploys", 20));
    const int rollbacks = std::max(0, takeIntOption(args, "--rollbacks", 5));
    if (args.size() < 6) {
        return usageError("station stress <工位ID> <上线包A> <上线包B> <图像或目录> [--count N] [--deploys N] [--rollbacks N]");
    }
    const std::string stationId = args[2], releaseA = args[3], releaseB = args[4];
    auto images = loadImages({args.begin() + 5, args.end()});
    if (!images) return fail(images.error());

    // 每个上线包对应的方案版本：用来核对检测有没有“串”到别的上线包上
    std::map<std::string, std::string> versionOf;
    for (const auto& id : {releaseA, releaseB}) {
        auto manifest = app.releases.getManifest(id);
        if (!manifest) return fail(manifest.error());
        versionOf[id] = manifest.value().recipeVersionId;
    }

    // 准备：让工位以上线包 A 运行
    auto view = app.stations.getStation(stationId);
    if (!view) return fail(view.error());
    if (view.value().activeRelease != releaseA) {
        if (auto st = app.stations.deploy(stationId, releaseA); !st) return fail(st.error());
    }
    if (auto st = startStation(app, stationId); !st) return fail(st.error());
    // 之前的生效上线包会被第一次检测换成 A；先做一次，让后面的统计从干净的状态开始
    if (auto warm = app.stations.inspect(stationId, images.value()[0]); !warm) return fail(warm.error());

    const Memory before = processMemory();
    std::atomic<int> done{0};
    std::atomic<int> deployed{0}, rolledBack{0}, commandFailures{0};
    std::vector<double> loadMs;  // 只由控制线程写，结束后才读

    std::thread controller([&] {
        const int steps = deployments + rollbacks;
        for (int step = 1; step <= steps; ++step) {
            const int at = static_cast<int>(static_cast<long long>(step) * (total - total / 20) / steps);
            while (done.load() < at) std::this_thread::sleep_for(std::chrono::microseconds(200));

            if (step % 5 == 0 && rolledBack.load() < rollbacks && app.stations.rollback(stationId).ok()) {
                ++rolledBack;
                continue;
            }
            const auto current = app.stations.getStation(stationId);
            const std::string next = (current && current.value().activeRelease == releaseA) ? releaseB : releaseA;
            const auto t0 = Clock::now();
            if (!app.stations.deploy(stationId, next).ok()) {
                ++commandFailures;
                continue;
            }
            (void)app.stations.waitForLoading(stationId);
            loadMs.push_back(millisSince(t0));
            ++deployed;
        }
    });

    std::vector<double> inspectMs, swapMicros, logMicros;
    inspectMs.reserve(static_cast<std::size_t>(total));
    logMicros.reserve(static_cast<std::size_t>(total));
    int lost = 0, mixed = 0, outOfOrder = 0, swaps = 0;
    std::map<std::string, int> tally;
    std::uint64_t lastSequence = 0;
    double peakCurrentMb = before.currentMb;
    const auto runStart = Clock::now();

    for (int i = 0; i < total; ++i) {
        const Image& frame = images.value()[static_cast<std::size_t>(i) % images.value().size()];
        const auto t0 = Clock::now();
        auto r = app.stations.inspect(stationId, frame);
        inspectMs.push_back(millisSince(t0));
        if (!r) {
            ++lost;
            done.fetch_add(1);
            continue;
        }
        const auto& v = r.value();
        if (lastSequence != 0 && v.sequence != lastSequence + 1) ++outOfOrder;
        lastSequence = v.sequence;
        if (v.swapped) {
            ++swaps;
            swapMicros.push_back(v.swapMicros);
        }
        const auto expected = versionOf.find(v.releaseId);
        if (expected == versionOf.end() || v.report.recipeVersionId != expected->second) ++mixed;
        ++tally[v.report.verdict];

        // 在检测线程里写一条日志要多久：日志是同步写文件的，这里量的就是它对检测线程的影响
        const auto l0 = Clock::now();
        log::info("stress", "一次检测", {{"seq", std::to_string(v.sequence)}, {"verdict", v.report.verdict}, {"release", v.releaseId}});
        logMicros.push_back(std::chrono::duration<double, std::micro>(Clock::now() - l0).count());

        if (i % 200 == 0) peakCurrentMb = std::max(peakCurrentMb, processMemory().currentMb);
        done.fetch_add(1);
    }
    const double seconds = millisSince(runStart) / 1000.0;
    controller.join();
    (void)app.stations.waitForLoading(stationId);
    (void)app.stations.stop(stationId);
    const Memory after = processMemory();

    std::cout << "检测 " << total << " 次，用时 " << std::fixed << std::setprecision(1) << seconds << " 秒（每秒 "
              << static_cast<int>(total / seconds) << " 次）；图像 " << images.value()[0].width() << "×" << images.value()[0].height() << "\n";
    std::cout << "期间：部署 " << deployed.load() << " 次，热切换 " << swaps << " 次，回滚 " << rolledBack.load() << " 次\n";
    std::cout << "判定：OK " << tally["OK"] << "，NG " << tally["NG"] << "，ERROR " << tally["ERROR"] << "\n\n";

    std::cout << "检查\n";
    std::cout << "  丢失或失败的检测：" << lost << "\n";
    std::cout << "  用了别的上线包的方案的检测：" << mixed << "\n";
    std::cout << "  序号不连续：" << outOfOrder << "\n";
    std::cout << "  被拒绝的部署：" << commandFailures.load() << "\n\n";

    std::cout << "测量\n";
    printDistribution("单次检测耗时", summarize(inspectMs), "毫秒");
    printDistribution("热切换本身的耗时", summarize(swapMicros), "微秒");
    printDistribution("上线包从部署到就绪", summarize(loadMs), "毫秒");
    printDistribution("检测线程里写一条日志", summarize(logMicros), "微秒");
    std::cout << std::setprecision(1) << "  内存：开始时 " << before.currentMb << " MB，过程中最高 " << std::max(peakCurrentMb, after.currentMb)
              << " MB，结束时 " << after.currentMb << " MB（进程启动以来的峰值 " << after.peakMb << " MB）\n";

    const bool passed = lost == 0 && mixed == 0 && outOfOrder == 0 && commandFailures.load() == 0;
    std::cout << "\n" << (passed ? "通过：没有检测丢失、中断或用错上线包" : "没有通过") << "\n";
    return passed ? 0 : 1;
}

}  // namespace

// ---------- release … ----------

int runReleaseCommand(platform::LinePlatform& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
    if (sub == "create" && args.size() >= 3) {
        auto id = app.releases.createRelease(args[2]);
        if (!id) return fail(id.error());
        std::cout << id.value() << "\n";
        return 0;
    }
    if (sub == "list") {
        for (const auto& id : app.releases.listReleases()) {
            const auto m = app.releases.getManifest(id);
            std::cout << id << "  " << (m ? m.value().recipeVersionId : "?") << (m && m.value().withdrawn ? "  （已撤回）" : "") << "\n";
        }
        return 0;
    }
    if (sub == "show" && args.size() >= 3) {
        auto m = app.releases.getManifest(args[2]);
        if (!m) return fail(m.error());
        std::cout << "上线包 " << m.value().releaseId << "\n  方案版本  " << m.value().recipeVersionId << "\n  状态      "
                  << (m.value().withdrawn ? "已撤回" : "有效") << "\n  锁定的模型";
        if (m.value().lockedModels.empty()) std::cout << "  （无）";
        std::cout << "\n";
        for (const auto& [slot, model] : m.value().lockedModels) std::cout << "    " << slot << " → " << model << "\n";
        return 0;
    }
    if (sub == "withdraw" && args.size() >= 3) {
        auto st = app.releases.withdraw(args[2]);
        return st ? 0 : fail(st.error());
    }
    return usageError("release create <方案版本ID> | list | show <上线包ID> | withdraw <上线包ID>");
}

// ---------- station … ----------

int runStationCommand(platform::LinePlatform& app, const std::vector<std::string>& rawArgs) {
    std::vector<std::string> args = rawArgs;
    const std::string sub = args[1];

    if (sub == "create") {
        station::StationOptions options;
        options.autoRollbackAfterErrors = takeIntOption(args, "--auto-rollback", options.autoRollbackAfterErrors);
        if (args.size() < 3) return usageError("station create <名称> [--auto-rollback N]");
        auto id = app.stations.createStation(args[2], options);
        if (!id) return fail(id.error());
        std::cout << id.value() << "\n";
        return 0;
    }
    if (sub == "list") {
        for (const auto& id : app.stations.listStations()) {
            const auto v = app.stations.getStation(id);
            std::cout << id << "  " << (v ? v.value().name : "?") << "\n";
        }
        return 0;
    }
    if (sub == "show" && args.size() >= 3) {
        // 上次运行时如果留下了还没切换的部署，第一次用到这个工位时会在后台重新加载；等它结束再显示
        if (auto st = app.stations.waitForLoading(args[2]); !st) return fail(st.error());
        auto v = app.stations.getStation(args[2]);
        if (!v) return fail(v.error());
        printStation(v.value());
        return 0;
    }
    if (sub == "deploy" && args.size() >= 4) {
        if (auto st = app.stations.deploy(args[2], args[3]); !st) return fail(st.error());
        if (auto st = app.stations.waitForLoading(args[2]); !st) return fail(st.error());
        auto v = app.stations.getStation(args[2]);
        if (!v) return fail(v.error());
        if (v.value().pendingRelease != args[3] || !v.value().pendingReady) {
            std::cerr << "错误：上线包加载失败，工位继续使用当前的上线包。原因见日志。\n";
            return 1;
        }
        std::cout << "上线包已就绪，将在下一次检测开始前生效\n";
        return 0;
    }
    if (sub == "rollback") {
        const bool withdraw = takeFlag(args, "--withdraw");
        if (args.size() < 3) return usageError("station rollback <工位ID> [--withdraw]");
        if (auto st = app.stations.rollback(args[2], withdraw); !st) return fail(st.error());
        auto v = app.stations.getStation(args[2]);
        if (v) std::cout << "已回滚到上线包 " << v.value().activeRelease << (withdraw ? "，并撤回了出问题的上线包" : "") << "\n";
        return 0;
    }
    if (sub == "bind" && args.size() >= 5) {
        // station bind <工位ID> folder <目录>
        const auto st = app.stations.bindImageSource(args[2], args[3], args[4]);
        if (!st) return fail(st.error());
        std::cout << "已绑定图像源 " << args[3] << "：" << args[4] << "\n";
        return 0;
    }
    if (sub == "unbind" && args.size() >= 3) {
        const auto st = app.stations.unbindImageSource(args[2]);
        return st ? 0 : fail(st.error());
    }
    if (sub == "run") return cmdStationRun(app, args);
    if (sub == "stress") return cmdStationStress(app, args);
    return usageError("station create | list | show | deploy | rollback | run | stress（详见 vision-cli help）");
}

// ---------- package ----------

int runPackageCommand(platform::LinePlatform& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
    const auto printManifest = [](const release::PackageManifest& m) {
        std::cout << "方案包 " << m.packageId << "\n  方案版本 " << m.recipeVersionId << "（" << m.recipeName << "）\n  算子";
        for (const auto& [id, major] : m.operators) std::cout << " " << id << "@v" << major;
        std::cout << "\n";
        for (const auto& t : m.templates) std::cout << "  匹配模板 " << t.name << "（" << t.bytes << " 字节）\n";
        for (const auto& x : m.models) std::cout << "  模型 " << x.id << "（" << x.kind << "，" << x.bytes << " 字节）\n";
    };
    if (sub == "export" && args.size() >= 4) {
        auto r = app.packages.exportPackage(args[2], args[3]);
        if (!r) return fail(r.error());
        std::cout << "已导出 " << args[3] << "：方案版本 " << r.value().recipeVersionId << "，匹配模板 " << r.value().templates << " 个，模型 "
                  << r.value().models << " 个\n";
        return 0;
    }
    if (sub == "import") {
        auto r = app.packages.importPackage(args[2]);
        if (!r) return fail(r.error());
        const auto& v = r.value();
        if (v.nothingNew()) {
            std::cout << "本平台已经有这个方案包里的全部内容，没有什么要导入的\n";
        } else {
            std::cout << "已导入方案版本 " << v.recipeVersionId << "（" << v.recipeName << "）" << (v.recipeAdded ? "" : "：本平台已经有了") << "\n";
            for (const auto& id : v.modelsAdded) std::cout << "  新增模型 " << id << "\n";
        }
        for (const auto& id : v.modelsAlreadyHere) std::cout << "  模型 " << id << " 本平台已经有了\n";
        std::cout << v.recipeVersionId << "\n";
        return 0;
    }
    if (sub == "show") {
        auto m = app.packages.inspect(args[2]);
        if (!m) return fail(m.error());
        printManifest(m.value());
        return 0;
    }
    std::cerr << "未知的 package 命令\n";
    return 1;
}

// ---------- model（只看）----------

int runModelViewCommand(platform::LinePlatform& app, const std::vector<std::string>& args) {
    const std::string sub = args.size() > 1 ? args[1] : "";
    if (sub == "list") {
        for (const auto& id : app.models.listModelVersions()) {
            const auto v = app.models.getModelVersion(id).value();
            std::cout << v.id << "  " << v.status << "  " << v.contract.kind << "  " << v.provenance.kind << "\n";
        }
        return 0;
    }
    if (sub == "show" && args.size() >= 3) {
        auto view = app.models.getModelVersion(args[2]);
        if (!view) return fail(view.error());
        const auto& v = view.value();
        std::cout << v.id << "  " << v.status << "  " << v.bytes << " 字节\n  契约：" << v.contract.kind << "，输入 " << v.contract.inputWidth
                  << "×" << v.contract.inputHeight << "×" << v.contract.inputChannels << "\n";
        for (const auto& [name, value] : v.contract.numbers) std::cout << "    " << name << " = " << value << "\n";
        if (v.provenance.kind == "trained") {
            std::cout << "  来源：训练任务 " << v.provenance.trainingJobId << "，数据集版本 " << v.provenance.datasetVersionId << "\n";
        }
        return 0;
    }
    return usageError("model list | show <模型版本ID>（产线运行平台上的模型只能由方案包带来）");
}

// ---------- serve：让工位持续运行 ----------
// 每个工位一个线程：取图、检测、取图、检测……图像源暂时没有新图像时歇一会儿再问。
// 工位服务本身不开这种线程：什么时候取下一帧是“驱动”它的程序的事（docs/ddd/13-two-programs.md §3）。

int serveStations(platform::LinePlatform& app, std::vector<std::string> stationIds, int exitWhenIdleSeconds,
                  const std::atomic<bool>& stopRequested) {
    // 没有指定工位：所有绑定了图像源的
    if (stationIds.empty()) {
        for (const auto& id : app.stations.listStations()) {
            if (auto v = app.stations.getStation(id); v && !v.value().imageSourceKind.empty()) stationIds.push_back(id);
        }
        if (stationIds.empty()) {
            std::cerr << "没有绑定了图像源的工位。先用 station bind 给工位绑定图像源\n";
            return 1;
        }
    }

    std::mutex printMutex;  // 几个工位的输出不要搅在一起
    std::map<std::string, std::string> names;
    platform::StationRunner runner(app);
    runner.onInspection = [&](const std::string& id, const platform::InspectionRecord& r) {
        std::lock_guard<std::mutex> lock(printMutex);
        if (r.swapped) std::cout << "  —— 工位 " << names[id] << " 已切换到上线包 " << shortId(r.releaseId) << " ——\n";
        std::cout << names[id] << " #" << r.sequence << " [" << r.verdict << "] " << r.frameName;
        for (const auto& reason : r.reasons) {
            if (!reason.pass) std::cout << "  ✗ " << reason.text;
        }
        std::cout << std::endl;
    };
    runner.onStopped = [&](const std::string& id, const std::string& reason) {
        std::lock_guard<std::mutex> lock(printMutex);
        std::cout << "工位 " << names[id] << " 停下了：" << reason << std::endl;
    };

    // 把每个工位启动起来（热启动：加载上次生效的上线包）。有一个启动不了就都不开始
    for (const auto& id : stationIds) {
        if (auto v = app.stations.getStation(id)) names[id] = v.value().name;
        if (auto st = runner.start(id); !st) {
            runner.stopAll();
            std::cerr << "工位 " << id << " 启动不了：";
            return fail(st.error());
        }
        const auto v = app.stations.getStation(id).value();
        std::lock_guard<std::mutex> lock(printMutex);
        std::cout << "工位 " << v.name << " 已启动：上线包 " << shortId(v.activeRelease) << "，图像源 " << v.imageSourceKind << "："
                  << v.imageSourceLocation << std::endl;
    }

    // 等停止的请求；或者（给了 exitWhenIdleSeconds）所有工位都闲了这么久；或者所有工位都自己停下了
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stopRequested || !runner.anyRunning()) break;
        if (exitWhenIdleSeconds > 0 && runner.idleFor() >= std::chrono::seconds(exitWhenIdleSeconds)) break;
    }
    runner.stopAll();

    for (const auto& id : stationIds) {
        const auto s = runner.status(id);
        std::cout << "工位 " << names[id] << " 合计：OK " << s.ok << "，NG " << s.ng << "，ERROR " << s.error;
        if (s.unreadable > 0) std::cout << "；读不出来的图像 " << s.unreadable << " 幅";
        std::cout << "\n";
    }
    return 0;
}

}  // namespace vision::cli
