#include "station_runner.h"

namespace vision::platform {

namespace {
constexpr std::size_t kKeptRecords = 100;  // 每个工位只留最近这么多条检测记录
using Clock = std::chrono::steady_clock;
}  // namespace

Status StationRunner::start(const std::string& stationId) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& slot = entries_[stationId];
        if (slot && slot->running) return Status::success();
        if (slot && slot->thread.joinable()) slot->thread.join();  // 上一次已经自己停下了：收尾
        slot = std::make_shared<Entry>();
        entry = slot;
    }
    // 热启动：等后台的加载结束，再启动。工位已经在运行（别处启动的）也可以
    if (auto st = platform_.stations.waitForLoading(stationId); !st) return st;
    if (auto st = platform_.stations.start(stationId); !st && st.error().code != "station.already_running") return st;
    entry->running = true;
    lastActivity_ = Clock::now().time_since_epoch().count();
    entry->thread = std::thread([this, stationId, entry] { loop(stationId, *entry); });
    return Status::success();
}

void StationRunner::loop(const std::string& stationId, Entry& entry) {
    while (!entry.quit) {
        auto r = platform_.stations.inspectNext(stationId);
        if (entry.quit) break;
        if (!r) {
            if (r.error().code == "image_source.bad_frame") {  // 一帧读不出来：记一笔，接着取后面的
                {
                    std::lock_guard<std::mutex> lock(entry.mutex);
                    ++entry.unreadable;
                }
                log::warn("runner", "图像读不出来", {{"station", stationId}, {"message", r.error().message}});
                continue;
            }
            // 工位不在运行了（被停掉、连续出错进入故障状态、图像源没了……）：到此为止
            {
                std::lock_guard<std::mutex> lock(entry.mutex);
                entry.stoppedReason = r.error().message;
            }
            log::error("runner", "工位停下了", {{"station", stationId}, {"code", r.error().code}, {"message", r.error().message}});
            if (onStopped) onStopped(stationId, r.error().message);
            break;
        }
        if (!r.value()) {  // 现在没有新图像：歇一会儿再问
            entry.idle = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        entry.idle = false;
        lastActivity_ = Clock::now().time_since_epoch().count();
        const auto& v = *r.value();
        InspectionRecord record;
        record.sequence = v.sequence;
        record.frameName = v.frameName;
        record.verdict = v.report.verdict;
        record.releaseId = v.releaseId;
        record.swapped = v.swapped;
        record.totalMs = v.report.totalMs;
        record.reasons = v.report.reasons;
        record.result = v.report.result;
        {
            std::lock_guard<std::mutex> lock(entry.mutex);
            ++(record.verdict == "OK" ? entry.ok : record.verdict == "NG" ? entry.ng : entry.error);
            entry.latest = v.frame;
            entry.recent.push_back(record);
            while (entry.recent.size() > kKeptRecords) entry.recent.pop_front();
        }
        if (onInspection) onInspection(stationId, record);
    }
    entry.idle = false;
    entry.running = false;
}

Status StationRunner::stop(const std::string& stationId) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(stationId);
        if (it == entries_.end()) return Status::success();
        entry = it->second;
    }
    entry->quit = true;
    (void)platform_.stations.stop(stationId);  // 工位一停，卡在取图上的线程也会回来
    if (entry->thread.joinable()) entry->thread.join();
    return Status::success();
}

void StationRunner::stopAll() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, entry] : entries_) ids.push_back(id);
    }
    for (const auto& id : ids) (void)stop(id);
}

RunnerStatus StationRunner::status(const std::string& stationId, std::uint64_t afterSequence) const {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(stationId);
        if (it == entries_.end()) return {};
        entry = it->second;
    }
    RunnerStatus s;
    s.running = entry->running;
    s.idle = entry->idle;
    std::lock_guard<std::mutex> lock(entry->mutex);
    s.ok = entry->ok;
    s.ng = entry->ng;
    s.error = entry->error;
    s.unreadable = entry->unreadable;
    s.stoppedReason = entry->stoppedReason;
    for (const auto& r : entry->recent) {
        if (r.sequence > afterSequence) s.recent.push_back(r);
    }
    return s;
}

Image StationRunner::latestFrame(const std::string& stationId) const {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(stationId);
        if (it == entries_.end()) return {};
        entry = it->second;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->latest;
}

bool StationRunner::anyRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, entry] : entries_) {
        if (entry->running) return true;
    }
    return false;
}

std::chrono::milliseconds StationRunner::idleFor() const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, entry] : entries_) {
            if (entry->running && !entry->idle) return std::chrono::milliseconds(0);
        }
    }
    const Clock::time_point last{Clock::duration{lastActivity_.load()}};
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last);
}

}  // namespace vision::platform
