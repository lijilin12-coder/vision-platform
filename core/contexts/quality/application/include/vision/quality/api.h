#pragma once

// BC-10 质量分析的公开接口（目前只有检测记录这一部分）。

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"
#include "vision/kernel/image.h"
#include "vision/kernel/result_items.h"

namespace vision::quality {

class InspectionRecordRepository;  // 领域层的仓储接口，由组装根传入实现
class ReviewRepository;
class ReviewBook;

// ===== 端口 =====

// 时钟：现在是什么时候。做成端口是为了测试时能给出确定的时间
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::int64_t nowMs() = 0;  // 1970-01-01 UTC 以来的毫秒数
};

// 清理的结果
struct ImageCleanup {
    int expired = 0;    // 过了保留期清掉的
    int forSpace = 0;   // 磁盘快满了、提前清掉的（还没到期）
    bool spaceLow = false;  // 清完之后磁盘剩余空间仍然低于要求
};

// 检测图像的存档。短期缓存和长期保留是两块地方；图像的编号由存档自己定
class ImageArchive {
public:
    virtual ~ImageArchive() = default;
    // 存一幅图像，返回它的编号。longTerm：直接长期保留；否则进短期缓存
    virtual Result<std::string> store(const std::string& stationId, const std::string& recordId, const Image& image, bool longTerm) = 0;
    // 这幅图像现在的情况：kept 长期保留着 / cached 在短期缓存里 / expired 已经清掉了
    virtual std::string state(const std::string& stationId, const std::string& imageId) = 0;
    // 图像的内容（PNG）
    virtual Result<std::vector<unsigned char>> load(const std::string& stationId, const std::string& imageId) = 0;
    // 把短期缓存里的一幅转成长期保留。已经是长期的、已经过期的：不算出错
    virtual Status keep(const std::string& stationId, const std::string& imageId) = 0;
    // 清理短期缓存：cutoffMs 之前的清掉；磁盘剩余空间低于 minFreeBytes 时从最旧的开始再清，直到够或者清空
    virtual ImageCleanup cleanup(std::int64_t cutoffMs, std::uint64_t minFreeBytes) = 0;
};

// ===== DTO =====

struct ReasonView {
    std::string nodeId;
    bool pass = true;
    std::string text;
};

// 要记下来的一次检测（由工位那边交过来）
struct InspectionToRecord {
    std::string stationId;
    std::uint64_t sequence = 0;
    std::string releaseId, recipeVersionId, frameName, verdict;
    std::vector<ReasonView> reasons;
    ResultItems result;
    double totalMs = 0;
    bool swapped = false;
    Image frame;  // 这一帧的图像；可以是空的
};

// 一条检测记录上最近的一次复判
struct ReviewView {
    bool truthNg = false;  // 人确认的真实情况：是否不合格
    std::string kind;      // correct 判对了 / false_alarm 误报 / miss 漏检 / was_error 系统当时出错
    std::string reviewer, note;
    std::int64_t timeMs = 0;
    std::string time;
};

struct RecordView {
    std::string id, stationId;
    std::int64_t timeMs = 0;
    std::string time;  // 给人看的时间（UTC），例如 "2026-11-24 09:15:02"
    std::uint64_t sequence = 0;
    std::string releaseId, recipeVersionId, frameName, imageId, verdict;
    // 图像现在的情况：none 当时就没留 / cached 在短期缓存里（过了保留期会清掉）/ kept 长期保留着 / expired 已经清掉了
    std::string imageState = "none";
    std::vector<ReasonView> reasons;
    ResultItems result;
    double totalMs = 0;
    bool swapped = false;
    std::optional<ReviewView> review;  // 还没有复判过时为空
};

struct RecordQuery {
    std::string verdict;   // 只要这种判定；空表示都要
    std::string review;    // 按复判筛选：空 都要 / unreviewed 还没复判的 / misjudged 复判确认判错了的（误报、漏检）
    std::string beforeId;  // 翻页：只要比它更早的
    int limit = 50;
};

struct SummaryView {
    long total = 0, ok = 0, ng = 0, error = 0;
    std::optional<double> yield;  // 良率：OK ÷ (OK + NG)
    std::map<std::string, long> defectsByCategory;
    // 复判：复判过多少条，其中判对了的、误报、漏检、系统当时出错的各多少
    long reviewed = 0, reviewedCorrect = 0, falseAlarms = 0, misses = 0, reviewedErrors = 0;
};

// 复判时提交的内容
struct ReviewRequest {
    bool truthNg = false;
    std::string reviewer, note;
};

// 哪些检测的图像要留下来（docs/ddd/18-image-cache.md）。图像比记录大得多，不能都永远留着。
enum class ImagePolicy {
    None,       // 都不留
    NgAndError, // 只留不合格的和出错的（长期）。判 OK 的不留——漏检事后无图可看
    All,        // 默认：不合格的和出错的长期保留；判 OK 的进短期缓存，过了保留期清掉
};

// ===== 应用服务 =====
// 可以从多个线程调用（几个工位同时在检测）。
class QualityService {
public:
    QualityService(InspectionRecordRepository& records, Clock& clock, ImageArchive& images, ImagePolicy policy = ImagePolicy::All);
    ~QualityService();
    void setImagePolicy(ImagePolicy policy) { policy_ = policy; }
    // 短期缓存留多久（默认 72 小时）。1 小时到 366 天之外的值被拒绝
    Status setImageRetentionHours(double hours);
    // 磁盘剩余空间低于这个数时，清理会提前清掉最旧的缓存图像（默认 2 GB；0 表示不管）
    void setMinFreeBytes(std::uint64_t bytes) { minFreeBytes_ = bytes; }
    // 清理一次短期缓存。程序启动时、之后每隔一段时间调用（组装根负责）
    ImageCleanup cleanExpiredImages();
    // 一条检测记录的图像（PNG）。当时没留、已经过期：返回失败，原因说清楚
    Result<std::vector<unsigned char>> recordImage(const std::string& stationId, const std::string& recordId) const;

    // ---- 复判（QUA-01，docs/ddd/17-review.md）----
    // 需要复判的存储，和发布事件的地方；组装根传入
    void useReviews(ReviewRepository& reviews, EventPublisher& events);
    // 复判一条检测记录：人确认它实际是合格还是不合格。可以重复复判，以最近的一次为准（R3）。
    // 确认是误判（误报、漏检）时发布 MisjudgmentConfirmed
    Result<RecordView> review(const std::string& stationId, const std::string& recordId, const ReviewRequest& request);

    // 记下一次检测。返回记录的编号。图像存不下来不算失败：记录照样写，只是没有图像
    Result<std::string> record(const InspectionToRecord& inspection);

    // 从新到旧
    std::vector<RecordView> query(const std::string& stationId, const RecordQuery& query = {}) const;
    Result<RecordView> getRecord(const std::string& stationId, const std::string& recordId) const;
    SummaryView summary(const std::string& stationId) const;

private:
    InspectionRecordRepository& records_;
    Clock& clock_;
    ImageArchive& images_;
    std::atomic<ImagePolicy> policy_;
    std::atomic<std::int64_t> retentionMs_{72LL * 3600 * 1000};
    std::atomic<std::uint64_t> minFreeBytes_{2ULL * 1024 * 1024 * 1024};
    ReviewRepository* reviews_ = nullptr;
    EventPublisher* events_ = nullptr;
    // 每个工位的复判，按“每条记录最近的一次”整理好。第一次用到时从存储里读
    mutable std::map<std::string, std::shared_ptr<ReviewBook>> books_;
    ReviewBook& book(const std::string& stationId) const;  // 调用时必须拿着 mutex_
    mutable std::mutex mutex_;  // 仓储的实现不要求线程安全
    std::int64_t lastTimeMs_ = 0;
    std::uint64_t counter_ = 0;  // 同一毫秒里的第几条
    // 每个工位的汇总。第一次问到时把这个工位的记录全部读一遍，之后每记一条就加一条——
    // 界面每秒都会来问一次，不能每次都把几万条记录重新读一遍。
    struct Totals;
    mutable std::map<std::string, std::shared_ptr<Totals>> totals_;
};

}  // namespace vision::quality
