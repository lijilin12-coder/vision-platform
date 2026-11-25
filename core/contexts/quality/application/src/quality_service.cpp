#include "vision/quality/api.h"

#include <cstdio>
#include <ctime>

#include "vision/common/log.h"
#include "vision/quality/domain/image_retention.h"
#include "vision/quality/domain/inspection_record.h"
#include "vision/quality/domain/review.h"

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

RecordView toView(const InspectionRecord& r, const Review* review, ImageArchive& images) {
    RecordView v;
    v.id = r->id;
    v.stationId = r->stationId;
    v.timeMs = r->timeMs;
    v.time = formatTime(r->timeMs);
    v.sequence = r->sequence;
    v.releaseId = r->releaseId;
    v.recipeVersionId = r->recipeVersionId;
    v.frameName = r->frameName;
    v.imageId = r->imageId;
    v.imageState = r->imageId.empty() ? "none" : images.state(r->stationId, r->imageId);
    v.verdict = r->verdict;
    for (const auto& reason : r->reasons) v.reasons.push_back({reason.nodeId, reason.pass, reason.text});
    v.result = r->result;
    v.totalMs = r->totalMs;
    v.swapped = r->swapped;
    if (review != nullptr) {
        const auto& f = review->fields();
        v.review = ReviewView{f.truthNg, reviewKindName(review->kind()), f.reviewer, f.note, f.timeMs, formatTime(f.timeMs)};
    }
    return v;
}

}  // namespace

struct QualityService::Totals {
    Summary summary;
};

QualityService::QualityService(InspectionRecordRepository& records, Clock& clock, ImageArchive& images, ImagePolicy policy)
    : records_(records), clock_(clock), images_(images), policy_(policy) {}

QualityService::~QualityService() = default;

void QualityService::useReviews(ReviewRepository& reviews, EventPublisher& events) {
    reviews_ = &reviews;
    events_ = &events;
}

ReviewBook& QualityService::book(const std::string& stationId) const {
    auto& b = books_[stationId];
    if (!b) {
        b = std::make_shared<ReviewBook>();
        if (reviews_ != nullptr) {
            for (const auto& review : reviews_->all(stationId)) b->add(review);
        }
    }
    return *b;
}

Result<RecordView> QualityService::review(const std::string& stationId, const std::string& recordId, const ReviewRequest& request) {
    using R = Result<RecordView>;
    if (reviews_ == nullptr) return R::failure("quality.no_reviews", "这个程序没有接上复判的存储");
    EventPtr event;
    RecordView view;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto record = records_.find(stationId, recordId);  // R1
        if (!record) return R::failure("quality.record_not_found", "找不到这条检测记录");
        Review::Fields f;
        f.recordId = recordId;
        f.stationId = stationId;
        f.verdict = (*record)->verdict;
        f.truthNg = request.truthNg;
        f.reviewer = request.reviewer;
        f.note = request.note;
        f.timeMs = clock_.nowMs();
        auto review = Review::create(std::move(f));
        if (!review) return review.error();
        ReviewBook& reviews = book(stationId);  // 先读出已有的，再追加：不然刚写的这条会被读进来再算一遍
        if (auto st = reviews_->append(review.value()); !st) return st.error();
        reviews.add(review.value());
        // M3：复判过的检测，图像转成长期保留。转不成（磁盘问题）不让复判失败，只留一条日志
        if (!(*record)->imageId.empty()) {
            if (auto st = images_.keep(stationId, (*record)->imageId); !st) {
                log::warn("quality", "复判过的图像没有转成长期保留", {{"station", stationId}, {"message", st.error().message}});
            }
        }
        if (review.value().misjudged()) {
            event = std::make_shared<MisjudgmentConfirmed>(stationId, recordId, reviewKindName(review.value().kind()),
                                                           (*record)->imageId);
        }
        view = toView(*record, reviews.latest(recordId), images_);
    }
    if (event && events_ != nullptr) events_->publish(event);  // 在锁外发布
    return view;
}

Result<std::string> QualityService::record(const InspectionToRecord& in) {
    // 先定下这条记录的编号（图像的文件要用它起名）。
    // 编号按时间递增。时钟偶尔会往回走（系统对时）：那时沿用上一条的时间，保证编号不倒退
    std::int64_t now = 0;
    std::string recordId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        now = clock_.nowMs();
        if (now <= lastTimeMs_) {
            now = lastTimeMs_;
            ++counter_;
        } else {
            lastTimeMs_ = now;
            counter_ = 0;
        }
        recordId = makeRecordId(now, counter_);
    }

    // 图像：按保留规则决定留不留、留多久（M1、M2）。存图像比较慢，放在锁外面做
    std::string imageId;
    const ImagePolicy policy = policy_;
    ImageRetention retention;
    retention.keepRejected = policy != ImagePolicy::None;
    retention.cacheAccepted = policy == ImagePolicy::All;
    const ImageKeeping keeping = retention.at(in.verdict);
    if (keeping != ImageKeeping::Discard && !in.frame.empty()) {
        auto stored = images_.store(in.stationId, recordId, in.frame, keeping == ImageKeeping::LongTerm);
        if (stored) {
            imageId = std::move(stored).value();
        } else {
            // 图像没存下来（磁盘满了？）：记录照样要写——知道“这一次判了 NG”比什么都没有强
            log::warn("quality", "检测的图像没有存下来", {{"station", in.stationId}, {"message", stored.error().message}});
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    InspectionRecord::Fields f;
    f.id = recordId;
    f.stationId = in.stationId;
    f.timeMs = now;
    f.sequence = in.sequence;
    f.releaseId = in.releaseId;
    f.recipeVersionId = in.recipeVersionId;
    f.frameName = in.frameName;
    f.imageId = std::move(imageId);
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
    const int limit = q.limit < 1 ? 1 : q.limit > 500 ? 500 : q.limit;
    RecordFilter filter;
    filter.verdict = q.verdict;
    filter.beforeId = q.beforeId;
    filter.limit = limit;
    std::lock_guard<std::mutex> lock(mutex_);
    const ReviewBook& reviews = book(stationId);
    std::vector<RecordView> views;
    // 按复判筛选是在这里做的（存储只认识检测记录）：一页一页往前取，直到凑够或者取完
    for (;;) {
        const auto page = records_.query(stationId, filter);
        for (const auto& r : page) {
            const Review* review = reviews.latest(r->id);
            if (q.review == "unreviewed" && review != nullptr) continue;
            if (q.review == "misjudged" && (review == nullptr || !review->misjudged())) continue;
            views.push_back(toView(r, review, images_));
            if (static_cast<int>(views.size()) >= limit) return views;
        }
        if (q.review.empty() || static_cast<int>(page.size()) < filter.limit) return views;
        filter.beforeId = page.back()->id;
    }
}

Result<RecordView> QualityService::getRecord(const std::string& stationId, const std::string& recordId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto r = records_.find(stationId, recordId);
    if (!r) return Result<RecordView>::failure("quality.record_not_found", "找不到这条检测记录");
    return toView(*r, book(stationId).latest(recordId), images_);
}

Status QualityService::setImageRetentionHours(double hours) {
    auto retention = ImageRetention::create(true, true, hours);
    if (!retention) return retention.error();
    retentionMs_ = retention.value().retentionMs;
    return Status::success();
}

ImageCleanup QualityService::cleanExpiredImages() {
    ImageRetention retention;
    retention.retentionMs = retentionMs_;
    // 不拿 mutex_：清理可能要删很多文件，不能挡住检测。存档自己保证“正在写的”和“正在删的”不打架
    const ImageCleanup done = images_.cleanup(retention.cutoff(clock_.nowMs()), minFreeBytes_);
    if (done.expired > 0) log::info("quality", "清理了过期的检测图像", {{"count", std::to_string(done.expired)}});
    if (done.forSpace > 0) {
        log::warn("quality", "磁盘快满了：提前清掉了还没到期的检测图像", {{"count", std::to_string(done.forSpace)}});
    }
    if (done.spaceLow) log::warn("quality", "磁盘剩余空间不足，而且短期缓存已经清空了。请清理磁盘");
    return done;
}

Result<std::vector<unsigned char>> QualityService::recordImage(const std::string& stationId, const std::string& recordId) const {
    using R = Result<std::vector<unsigned char>>;
    std::string imageId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto r = records_.find(stationId, recordId);
        if (!r) return R::failure("quality.record_not_found", "找不到这条检测记录");
        imageId = (*r)->imageId;
    }
    if (imageId.empty()) return R::failure("quality.image_not_found", "这一次检测当时没有留图像");
    if (images_.state(stationId, imageId) == "expired") {
        return R::failure("quality.image_not_found", "这幅图像已经过了保留期，被清掉了");
    }
    return images_.load(stationId, imageId);
}

SummaryView QualityService::summary(const std::string& stationId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& totals = totals_[stationId];
    if (!totals) totals = std::make_shared<Totals>(Totals{summarize(records_.all(stationId))});
    const Summary& s = totals->summary;
    const ReviewSummary& r = book(stationId).summary();
    return {s.total, s.ok, s.ng, s.error, s.yield(), s.defectsByCategory, r.reviewed, r.correct, r.falseAlarms, r.misses, r.wasError};
}

}  // namespace vision::quality
