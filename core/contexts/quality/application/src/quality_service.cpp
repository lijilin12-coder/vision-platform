#include "vision/quality/api.h"

#include <cstdio>
#include <ctime>

#include "vision/common/log.h"
#include "vision/quality/domain/inspection_record.h"

namespace vision::quality {

namespace {

// 毫秒时间 → "2026-11-24 09:15:02"（UTC）
std::string formatTime(std::int64_t timeMs) {
    const std::time_t seconds = static_cast<std::time_t>(timeMs / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char buffer[80];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec);
    return buffer;
}

RecordView toView(const InspectionRecord& r) {
    RecordView v;
    v.id = r->id;
    v.stationId = r->stationId;
    v.timeMs = r->timeMs;
    v.time = formatTime(r->timeMs);
    v.sequence = r->sequence;
    v.releaseId = r->releaseId;
    v.recipeVersionId = r->recipeVersionId;
    v.frameName = r->frameName;
    v.imageSha256 = r->imageSha256;
    v.verdict = r->verdict;
    for (const auto& reason : r->reasons) v.reasons.push_back({reason.nodeId, reason.pass, reason.text});
    v.result = r->result;
    v.totalMs = r->totalMs;
    v.swapped = r->swapped;
    return v;
}

}  // namespace

struct QualityService::Totals {
    Summary summary;
};

QualityService::QualityService(InspectionRecordRepository& records, Clock& clock, ImageArchive& images, ImagePolicy policy)
    : records_(records), clock_(clock), images_(images), policy_(policy) {}

Result<std::string> QualityService::record(const InspectionToRecord& in) {
    // 图像：按策略决定留不留。存图像比较慢，放在锁外面做
    std::string imageSha256;
    const bool keep = policy_ == ImagePolicy::All || (policy_ == ImagePolicy::NgAndError && in.verdict != "OK");
    if (keep && !in.frame.empty()) {
        auto stored = images_.store(in.frame);
        if (stored) {
            imageSha256 = std::move(stored).value();
        } else {
            // 图像没存下来（磁盘满了？）：记录照样要写——知道“这一次判了 NG”比什么都没有强
            log::warn("quality", "检测的图像没有存下来", {{"station", in.stationId}, {"message", stored.error().message}});
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // 编号按时间递增。时钟偶尔会往回走（系统对时）：那时沿用上一条的时间，保证编号不倒退
    std::int64_t now = clock_.nowMs();
    if (now <= lastTimeMs_) {
        now = lastTimeMs_;
        ++counter_;
    } else {
        lastTimeMs_ = now;
        counter_ = 0;
    }
    InspectionRecord::Fields f;
    f.id = makeRecordId(now, counter_);
    f.stationId = in.stationId;
    f.timeMs = now;
    f.sequence = in.sequence;
    f.releaseId = in.releaseId;
    f.recipeVersionId = in.recipeVersionId;
    f.frameName = in.frameName;
    f.imageSha256 = std::move(imageSha256);
    f.verdict = in.verdict;
    for (const auto& reason : in.reasons) f.reasons.push_back({reason.nodeId, reason.pass, reason.text});
    f.result = in.result;
    f.totalMs = in.totalMs;
    f.swapped = in.swapped;
    auto record = InspectionRecord::create(std::move(f));
    if (!record) return record.error();
    if (auto st = records_.append(record.value()); !st) return st.error();
    if (auto it = totals_.find(in.stationId); it != totals_.end()) it->second->summary.add(record.value());
    return record.value()->id;
}

std::vector<RecordView> QualityService::query(const std::string& stationId, const RecordQuery& q) const {
    RecordFilter filter;
    filter.verdict = q.verdict;
    filter.beforeId = q.beforeId;
    filter.limit = q.limit < 1 ? 1 : q.limit > 500 ? 500 : q.limit;
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecordView> views;
    for (const auto& r : records_.query(stationId, filter)) views.push_back(toView(r));
    return views;
}

Result<RecordView> QualityService::getRecord(const std::string& stationId, const std::string& recordId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto r = records_.find(stationId, recordId);
    if (!r) return Result<RecordView>::failure("quality.record_not_found", "找不到这条检测记录");
    return toView(*r);
}

SummaryView QualityService::summary(const std::string& stationId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& totals = totals_[stationId];
    if (!totals) totals = std::make_shared<Totals>(Totals{summarize(records_.all(stationId))});
    const Summary& s = totals->summary;
    return {s.total, s.ok, s.ng, s.error, s.yield(), s.defectsByCategory};
}

}  // namespace vision::quality
