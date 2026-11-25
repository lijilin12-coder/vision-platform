#include "vision/quality/domain/review.h"

namespace vision::quality {

const char* reviewKindName(ReviewKind kind) {
    switch (kind) {
        case ReviewKind::Correct: return "correct";
        case ReviewKind::FalseAlarm: return "false_alarm";
        case ReviewKind::Miss: return "miss";
        case ReviewKind::WasError: return "was_error";
    }
    return "correct";
}

Result<Review> Review::create(Fields f) {
    using R = Result<Review>;
    if (f.recordId.empty() || f.stationId.empty()) return R::failure("quality.review_invalid", "复判没有写明是哪个工位的哪一条检测记录");
    if (f.verdict != "OK" && f.verdict != "NG" && f.verdict != "ERROR") {
        return R::failure("quality.review_invalid", "复判里系统的判定只能是 OK、NG、ERROR");
    }
    if (f.timeMs <= 0) return R::failure("quality.review_invalid", "复判没有写明时间");
    return Review(std::move(f));
}

ReviewKind Review::kind() const {
    if (f_.verdict == "ERROR") return ReviewKind::WasError;
    const bool judgedNg = f_.verdict == "NG";
    if (judgedNg == f_.truthNg) return ReviewKind::Correct;
    return judgedNg ? ReviewKind::FalseAlarm : ReviewKind::Miss;
}

namespace {
long& counterFor(ReviewSummary& s, ReviewKind kind) {
    switch (kind) {
        case ReviewKind::Correct: return s.correct;
        case ReviewKind::FalseAlarm: return s.falseAlarms;
        case ReviewKind::Miss: return s.misses;
        case ReviewKind::WasError: return s.wasError;
    }
    return s.correct;
}
}  // namespace

void ReviewBook::add(const Review& review) {
    const auto it = latest_.find(review->recordId);
    if (it != latest_.end()) {
        // R3：同一条记录又复判了一次。之前那次不再算数
        --counterFor(summary_, it->second.kind());
        it->second = review;
    } else {
        latest_.emplace(review->recordId, review);
        ++summary_.reviewed;
    }
    ++counterFor(summary_, review.kind());
}

const Review* ReviewBook::latest(const std::string& recordId) const {
    const auto it = latest_.find(recordId);
    return it == latest_.end() ? nullptr : &it->second;
}

}  // namespace vision::quality
