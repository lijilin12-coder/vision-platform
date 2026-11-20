#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::station {

using StationId = std::string;
using ReleaseId = std::string;  // 本上下文只把上线包当作一个标识，不关心它的内容

enum class StationState { Stopped, Running, Paused, Faulted };
const char* toString(StationState s);

// 一次检测的结果在本上下文里的含义（由应用层从检测执行的判定翻译过来）
enum class Outcome { Ok, Ng, Error };

enum class RollbackCause { Manual, Auto };

// 值对象：工位的策略
struct StationPolicy {
    int autoRollbackAfterErrors = 3;  // 连续多少次 ERROR 自动回滚（规则 P7）
    int historyDepth = 5;             // 最多记住多少个可以回滚到的上线包
};

// 值对象：待切换的上线包
struct PendingRelease {
    ReleaseId releaseId;
    bool ready = false;  // 已经在后台加载、预热完成，可以切换
};

// 值对象：一次检测的“入场券”。检测开始时确定用哪个上线包，此后不再变化（S5、H-04）。
struct InspectionTicket {
    ReleaseId releaseId;
    std::uint64_t sequence = 0;  // 这是本工位的第几次检测
    bool swapped = false;        // 这次检测开始前是否刚刚发生了热切换
};

// 用于保存和恢复的快照
struct StationSnapshot {
    StationId id;
    std::string name;
    StationState state = StationState::Stopped;
    std::optional<ReleaseId> activeRelease;
    std::optional<PendingRelease> pendingRelease;
    std::vector<ReleaseId> releaseHistory;  // 从旧到新
    StationPolicy policy;
    int consecutiveErrors = 0;
    std::uint64_t sequence = 0;
};

// 聚合根：工位（docs/ddd/05-tactical-design.md §5.1）。
//
// 不变量：
//   S1 运行中（Running）时必须有生效的上线包
//   S2 最多一个待切换的上线包；新的部署替换尚未切换的旧部署
//   S3 只在检测开始前切换：切换只发生在 beginInspection() 里（H-04）；
//      工位没有在运行时，就绪的待切换上线包在 start() 时生效
//   S4 回滚的目标必须在历史中存在
//   S5 每次检测开始时确定它使用的上线包，此后不再变化
//   另：一个工位同一时刻只进行一次检测（D5-4）
//
// POC-9 没有实现的部分：相机与标定档案、质量偏离监控（S6）。
class Station : public EventRecorder {
public:
    static Result<Station> create(StationId id, std::string name, StationPolicy policy = {});
    static Station restore(StationSnapshot snapshot);
    StationSnapshot snapshot() const;

    // 程序重新启动之后调用：上次保存的“运行中”和“已就绪”在新的进程里都不再成立。
    // 生效的上线包、历史保持不变，所以接下来 start() 就是热启动。
    void recoverAfterRestart();

    // ---- 部署与切换 ----
    Status deploy(ReleaseId release);
    Status markPendingReady(const ReleaseId& release);
    Status markPendingFailed(const ReleaseId& release, const std::string& reason);
    Status rollback(bool withdrawRequested);

    // ---- 生命周期 ----
    Status start();
    Status pause();
    Status resume();
    Status stop();

    // ---- 检测 ----
    Result<InspectionTicket> beginInspection();
    Status recordOutcome(const InspectionTicket& ticket, Outcome outcome);

    // ---- 查询 ----
    const StationId& id() const { return id_; }
    const std::string& name() const { return name_; }
    StationState state() const { return state_; }
    const std::optional<ReleaseId>& activeRelease() const { return activeRelease_; }
    const std::optional<PendingRelease>& pendingRelease() const { return pendingRelease_; }
    const std::vector<ReleaseId>& releaseHistory() const { return releaseHistory_; }
    const StationPolicy& policy() const { return policy_; }
    int consecutiveErrors() const { return consecutiveErrors_; }
    std::uint64_t sequence() const { return sequence_; }

    // 自上次调用以来，需要持久化的状态有没有变化。应用层据此决定要不要保存。
    // （每次检测都会让序号加一，但只为这个不值得写一次磁盘。）
    bool takeDirty();

private:
    Station() = default;
    void activatePending();   // 把就绪的待切换上线包变成生效的
    void doRollback(RollbackCause cause, bool withdrawRequested);
    void pushHistory(ReleaseId release);
    void markDirty() { dirty_ = true; }

    StationId id_;
    std::string name_;
    StationState state_ = StationState::Stopped;
    std::optional<ReleaseId> activeRelease_;
    std::optional<PendingRelease> pendingRelease_;
    std::vector<ReleaseId> releaseHistory_;
    StationPolicy policy_;
    int consecutiveErrors_ = 0;
    std::uint64_t sequence_ = 0;
    bool inspecting_ = false;  // 有一次检测正在进行（不持久化）
    bool dirty_ = false;
};

// ---------- 领域事件 ----------

struct StationCreated final : DomainEvent {
    std::string stationId, stationName;
    StationCreated(std::string id, std::string n) : stationId(std::move(id)), stationName(std::move(n)) {}
    const char* name() const override { return "StationCreated"; }
};

struct StationStarted final : DomainEvent {
    std::string stationId, releaseId;
    StationStarted(std::string id, std::string r) : stationId(std::move(id)), releaseId(std::move(r)) {}
    const char* name() const override { return "StationStarted"; }
};

struct StationPaused final : DomainEvent {
    std::string stationId;
    explicit StationPaused(std::string id) : stationId(std::move(id)) {}
    const char* name() const override { return "StationPaused"; }
};

struct StationResumed final : DomainEvent {
    std::string stationId;
    explicit StationResumed(std::string id) : stationId(std::move(id)) {}
    const char* name() const override { return "StationResumed"; }
};

struct StationStopped final : DomainEvent {
    std::string stationId;
    explicit StationStopped(std::string id) : stationId(std::move(id)) {}
    const char* name() const override { return "StationStopped"; }
};

// 连续 ERROR 达到上限，又没有可以回滚到的上线包：工位停下来等人处理
struct StationFaulted final : DomainEvent {
    std::string stationId, reason;
    StationFaulted(std::string id, std::string r) : stationId(std::move(id)), reason(std::move(r)) {}
    const char* name() const override { return "StationFaulted"; }
};

struct ReleaseDeployed final : DomainEvent {
    std::string stationId, releaseId;
    std::string replacedPending;  // 被这次部署替换掉的、尚未切换的上线包；没有则为空
    ReleaseDeployed(std::string id, std::string r, std::string replaced)
        : stationId(std::move(id)), releaseId(std::move(r)), replacedPending(std::move(replaced)) {}
    const char* name() const override { return "ReleaseDeployed"; }
};

struct ReleaseReady final : DomainEvent {
    std::string stationId, releaseId;
    ReleaseReady(std::string id, std::string r) : stationId(std::move(id)), releaseId(std::move(r)) {}
    const char* name() const override { return "ReleaseReady"; }
};

struct ReleaseLoadFailed final : DomainEvent {
    std::string stationId, releaseId, reason;
    ReleaseLoadFailed(std::string id, std::string r, std::string why)
        : stationId(std::move(id)), releaseId(std::move(r)), reason(std::move(why)) {}
    const char* name() const override { return "ReleaseLoadFailed"; }
};

struct HotSwapped final : DomainEvent {
    std::string stationId;
    std::string fromRelease;  // 第一次生效时为空
    std::string toRelease;
    std::uint64_t firstSequence;  // 使用新上线包的第一次检测的序号
    HotSwapped(std::string id, std::string from, std::string to, std::uint64_t seq)
        : stationId(std::move(id)), fromRelease(std::move(from)), toRelease(std::move(to)), firstSequence(seq) {}
    const char* name() const override { return "HotSwapped"; }
};

struct RolledBack final : DomainEvent {
    std::string stationId, fromRelease, toRelease;
    RollbackCause cause;
    bool withdrawRequested;       // 是否要求同时撤回出问题的上线包（F-5），由应用层去执行
    std::string discardedPending; // 回滚时被丢弃的待切换上线包；没有则为空
    RolledBack(std::string id, std::string from, std::string to, RollbackCause c, bool withdraw, std::string discarded)
        : stationId(std::move(id)),
          fromRelease(std::move(from)),
          toRelease(std::move(to)),
          cause(c),
          withdrawRequested(withdraw),
          discardedPending(std::move(discarded)) {}
    const char* name() const override { return "RolledBack"; }
};

// ---------- 仓储 ----------

class StationRepository {
public:
    virtual ~StationRepository() = default;
    virtual Status save(const Station& station) = 0;
    virtual std::optional<Station> find(const StationId& id) = 0;
    virtual std::vector<StationId> listIds() = 0;
};

}  // namespace vision::station
