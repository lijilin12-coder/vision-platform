#pragma once

// BC-10 质量分析的公开接口（目前只有检测记录这一部分）。

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/result.h"
#include "vision/kernel/image.h"
#include "vision/kernel/result_items.h"

namespace vision::quality {

class InspectionRecordRepository;  // 领域层的仓储接口，由组装根传入实现

// ===== 端口 =====

// 时钟：现在是什么时候。做成端口是为了测试时能给出确定的时间
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::int64_t nowMs() = 0;  // 1970-01-01 UTC 以来的毫秒数
};

// 把一幅图像存起来，返回它的内容哈希（存到内容仓库里；同样的图像只存一份）
class ImageArchive {
public:
    virtual ~ImageArchive() = default;
    virtual Result<std::string> store(const Image& image) = 0;
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

struct RecordView {
    std::string id, stationId;
    std::int64_t timeMs = 0;
    std::string time;  // 给人看的时间（UTC），例如 "2026-11-24 09:15:02"
    std::uint64_t sequence = 0;
    std::string releaseId, recipeVersionId, frameName, imageSha256, verdict;
    std::vector<ReasonView> reasons;
    ResultItems result;
    double totalMs = 0;
    bool swapped = false;
};

struct RecordQuery {
    std::string verdict;   // 只要这种判定；空表示都要
    std::string beforeId;  // 翻页：只要比它更早的
    int limit = 50;
};

struct SummaryView {
    long total = 0, ok = 0, ng = 0, error = 0;
    std::optional<double> yield;  // 良率：OK ÷ (OK + NG)
    std::map<std::string, long> defectsByCategory;
};

// 哪些检测的图像要留下来。图像比记录大得多（一条记录几百字节，一幅图像几百 KB），不能都留。
enum class ImagePolicy {
    None,       // 都不留
    NgAndError, // 只留不合格的和出错的（默认）：出了问题要能回头看当时的图像
    All,        // 都留
};

// ===== 应用服务 =====
// 可以从多个线程调用（几个工位同时在检测）。
class QualityService {
public:
    QualityService(InspectionRecordRepository& records, Clock& clock, ImageArchive& images, ImagePolicy policy = ImagePolicy::NgAndError);

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
    ImagePolicy policy_;
    mutable std::mutex mutex_;  // 仓储的实现不要求线程安全
    std::int64_t lastTimeMs_ = 0;
    std::uint64_t counter_ = 0;  // 同一毫秒里的第几条
    // 每个工位的汇总。第一次问到时把这个工位的记录全部读一遍，之后每记一条就加一条——
    // 界面每秒都会来问一次，不能每次都把几万条记录重新读一遍。
    struct Totals;
    mutable std::map<std::string, std::shared_ptr<Totals>> totals_;
};

}  // namespace vision::quality
