#include "vision/quality/domain/inspection_record.h"

#include <algorithm>
#include <cstdio>

namespace vision::quality {

Result<InspectionRecord> InspectionRecord::create(Fields f) {
    using R = Result<InspectionRecord>;
    // Q2
    if (f.id.empty()) return R::failure("quality.record_invalid", "检测记录没有编号");
    if (f.stationId.empty()) return R::failure("quality.record_invalid", "检测记录没有写明是哪个工位");
    if (f.timeMs <= 0) return R::failure("quality.record_invalid", "检测记录没有写明时间");
    if (f.releaseId.empty() || f.recipeVersionId.empty()) {
        return R::failure("quality.record_invalid", "检测记录必须写明用的是哪个上线包、哪个方案版本：否则事后追不到当时是怎么检测的");
    }
    // Q3
    if (f.verdict != "OK" && f.verdict != "NG" && f.verdict != "ERROR") {
        return R::failure("quality.record_invalid", "检测记录的判定只能是 OK、NG、ERROR，不能是“" + f.verdict + "”");
    }
    if (f.verdict == "NG" && std::none_of(f.reasons.begin(), f.reasons.end(), [](const RecordedReason& r) { return !r.pass; })) {
        return R::failure("quality.record_invalid", "判 NG 的检测记录至少要有一条不合格的判定依据");
    }
    return InspectionRecord(std::move(f));
}

std::string makeRecordId(std::int64_t timeMs, std::uint64_t counter) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%013lld-%06llu", static_cast<long long>(timeMs), static_cast<unsigned long long>(counter % 1000000));
    return buffer;
}

std::optional<double> Summary::yield() const {
    if (ok + ng == 0) return std::nullopt;
    return static_cast<double>(ok) / static_cast<double>(ok + ng);
}

void Summary::add(const InspectionRecord& r) {
    ++total;
    ++(r->verdict == "OK" ? ok : r->verdict == "NG" ? ng : error);
    for (const auto& d : r->result.defects) ++defectsByCategory[d.category];
}

Summary summarize(const std::vector<InspectionRecord>& records) {
    Summary s;
    for (const auto& r : records) s.add(r);
    return s;
}

}  // namespace vision::quality
