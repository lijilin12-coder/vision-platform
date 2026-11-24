#pragma once

// 让工位持续运行：每个工位一个线程，从它的图像源取图、检测，直到被停下。
// vision-line serve（命令行）和 vision-runtime（给界面用）共用这一份。
//
// 工位服务（应用层）只提供“取下一帧并检测”这一步；什么时候取下一帧是驱动它的程序的事——就是这里。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "line_platform.h"

namespace vision::platform {

// 一次检测留下的记录（给界面和命令行显示用；不是持久化的检测记录）
struct InspectionRecord {
    std::uint64_t sequence = 0;
    std::string frameName, verdict, releaseId;
    bool swapped = false;  // 这次检测开始前发生了热切换
    double totalMs = 0;
    std::vector<execution::ReasonReport> reasons;
    ResultItems result;
};

struct RunnerStatus {
    bool running = false;  // 取图的线程在不在跑
    bool idle = false;     // 在跑，但图像源现在没有新图像
    long ok = 0, ng = 0, error = 0, unreadable = 0;
    std::string stoppedReason;              // 不是我们让它停、它自己停下了：为什么
    std::vector<InspectionRecord> recent;   // 最近的检测，从旧到新
};

class StationRunner {
public:
    explicit StationRunner(LinePlatform& platform) : platform_(platform) {}
    ~StationRunner() { stopAll(); }
    StationRunner(const StationRunner&) = delete;
    StationRunner& operator=(const StationRunner&) = delete;

    // 启动工位（热启动：加载上次生效的上线包）并开始取图检测。已经在跑时什么都不做。
    Status start(const std::string& stationId);
    // 停下：正在进行的那一次检测会做完。没有在跑时什么都不做。
    Status stop(const std::string& stationId);
    void stopAll();

    // afterSequence：只要序号比它大的检测记录（界面每次只取新的）
    RunnerStatus status(const std::string& stationId, std::uint64_t afterSequence = 0) const;
    // 最近检测的那一帧图像；还没有时是空图像
    Image latestFrame(const std::string& stationId) const;
    // 所有在跑的工位都闲着（或者都停了），已经持续了多久。有工位在检测时是 0
    std::chrono::milliseconds idleFor() const;
    bool anyRunning() const;

    // 每完成一次检测调用一次（在工位的线程里）。要在 start 之前设置
    std::function<void(const std::string& stationId, const InspectionRecord&)> onInspection;
    // 工位自己停下了（不是我们让它停的）
    std::function<void(const std::string& stationId, const std::string& reason)> onStopped;

private:
    struct Entry {
        std::thread thread;
        std::atomic<bool> quit{false}, running{false}, idle{false};
        mutable std::mutex mutex;  // 保护下面这些
        long ok = 0, ng = 0, error = 0, unreadable = 0;
        std::string stoppedReason;
        std::deque<InspectionRecord> recent;
        Image latest;
    };
    void loop(const std::string& stationId, Entry& entry);

    LinePlatform& platform_;
    mutable std::mutex mutex_;  // 保护 entries_ 这张表本身
    std::map<std::string, std::shared_ptr<Entry>> entries_;
    std::atomic<std::chrono::steady_clock::rep> lastActivity_{std::chrono::steady_clock::now().time_since_epoch().count()};
};

}  // namespace vision::platform
