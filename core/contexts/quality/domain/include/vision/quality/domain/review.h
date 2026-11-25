#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::quality {

// 复判的结论：人确认的真实情况，对照系统当时的判定
enum class ReviewKind {
    Correct,     // 系统判对了
    FalseAlarm,  // 误报：系统判 NG，实际合格
    Miss,        // 漏检：系统判 OK，实际不合格
    WasError,    // 系统当时没检测下来（ERROR）；人补上了真实情况
};

const char* reviewKindName(ReviewKind kind);  // correct / false_alarm / miss / was_error

// 复判（术语表：复判 Review）：操作员人工确认系统对某一次检测的判定对不对。
//
// 检测记录是不可变的（Q1），所以复判不改记录，而是另外记一条，指向那条记录。
//
// 不变量：
//   R1 复判必须指向一条存在的检测记录（由应用层保证：先找到记录，再创建复判）
//   R2 真实情况只有合格 / 不合格两种。ERROR 不是一种真实情况（和基准图集的真实判定是同一个道理）
//   R3 复判只追加。人可以改主意：再复判一次，以最近的一次为准；之前的那次留着，查得到谁在什么时候怎么判的
//   R4 结论（判对 / 误报 / 漏检）由“系统的判定”和“真实情况”算出来，不由人另外填
class Review {
public:
    struct Fields {
        std::string recordId, stationId;
        std::string verdict;      // 系统当时的判定（从检测记录上抄来）：OK / NG / ERROR
        bool truthNg = false;     // 人确认的真实情况：是否不合格
        std::string reviewer;     // 谁复判的；可以为空（现在还没有登录）
        std::string note;         // 备注；可以为空
        std::int64_t timeMs = 0;
    };

    static Result<Review> create(Fields fields);  // 检查 R2 的前提和必填项
    const Fields& fields() const { return f_; }
    const Fields* operator->() const { return &f_; }
    ReviewKind kind() const;                       // R4
    bool misjudged() const { return kind() == ReviewKind::FalseAlarm || kind() == ReviewKind::Miss; }

private:
    explicit Review(Fields f) : f_(std::move(f)) {}
    Fields f_;
};

// 一个工位的复判汇总：每条记录只算它最近的一次复判（R3）
struct ReviewSummary {
    long reviewed = 0, correct = 0, falseAlarms = 0, misses = 0, wasError = 0;
};

// 一个工位的全部复判，按“每条记录最近的一次”整理好（R3）
class ReviewBook {
public:
    void add(const Review& review);  // 按时间先后的顺序加进来
    const Review* latest(const std::string& recordId) const;
    const ReviewSummary& summary() const { return summary_; }

private:
    std::map<std::string, Review> latest_;
    ReviewSummary summary_;
};

// 仓储：只追加
class ReviewRepository {
public:
    virtual ~ReviewRepository() = default;
    virtual Status append(const Review& review) = 0;
    virtual std::vector<Review> all(const std::string& stationId) = 0;  // 按写入的顺序
};

// 领域事件：复判确认了一次误判（误报或漏检）。数据那一侧可以据此把图像收作样本（规则 P3）
struct MisjudgmentConfirmed final : DomainEvent {
    std::string stationId, recordId, kind, imageSha256;
    MisjudgmentConfirmed(std::string s, std::string r, std::string k, std::string i)
        : stationId(std::move(s)), recordId(std::move(r)), kind(std::move(k)), imageSha256(std::move(i)) {}
    const char* name() const override { return "MisjudgmentConfirmed"; }
};

}  // namespace vision::quality
