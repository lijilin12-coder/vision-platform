#include "vision/station/domain/station.h"

namespace vision::station {

const char* toString(StationState s) {
    switch (s) {
        case StationState::Stopped: return "stopped";
        case StationState::Running: return "running";
        case StationState::Paused: return "paused";
        case StationState::Faulted: return "faulted";
    }
    return "?";
}

Result<Station> Station::create(StationId id, std::string name, StationPolicy policy) {
    using R = Result<Station>;
    if (id.empty()) return R::failure("station.empty_id", "工位 ID 不能为空");
    if (name.empty()) return R::failure("station.empty_name", "工位名称不能为空");
    if (policy.autoRollbackAfterErrors < 1) {
        return R::failure("station.bad_policy", "自动回滚的连续 ERROR 次数必须至少为 1");
    }
    if (policy.historyDepth < 1) return R::failure("station.bad_policy", "历史深度必须至少为 1");
    Station s;
    s.id_ = std::move(id);
    s.name_ = std::move(name);
    s.policy_ = policy;
    s.markDirty();
    s.record(std::make_shared<StationCreated>(s.id_, s.name_));
    return s;
}

Station Station::restore(StationSnapshot snap) {
    Station s;
    s.id_ = std::move(snap.id);
    s.name_ = std::move(snap.name);
    s.state_ = snap.state;
    s.activeRelease_ = std::move(snap.activeRelease);
    s.pendingRelease_ = std::move(snap.pendingRelease);
    s.releaseHistory_ = std::move(snap.releaseHistory);
    s.policy_ = snap.policy;
    s.consecutiveErrors_ = snap.consecutiveErrors;
    s.sequence_ = snap.sequence;
    return s;
}

StationSnapshot Station::snapshot() const {
    return {id_, name_, state_, activeRelease_, pendingRelease_, releaseHistory_, policy_, consecutiveErrors_, sequence_};
}

void Station::recoverAfterRestart() {
    if (state_ == StationState::Running || state_ == StationState::Paused) {
        state_ = StationState::Stopped;
        markDirty();
    }
    if (pendingRelease_ && pendingRelease_->ready) {
        pendingRelease_->ready = false;  // 加载好的内容随着旧进程一起没有了，需要重新加载
        markDirty();
    }
    inspecting_ = false;
}

bool Station::takeDirty() {
    const bool was = dirty_;
    dirty_ = false;
    return was;
}

void Station::pushHistory(ReleaseId release) {
    releaseHistory_.push_back(std::move(release));
    while (releaseHistory_.size() > static_cast<std::size_t>(policy_.historyDepth)) {
        releaseHistory_.erase(releaseHistory_.begin());
    }
}

// ---------------- 部署与切换 ----------------

Status Station::deploy(ReleaseId release) {
    if (release.empty()) return Status::failure("station.empty_release", "上线包 ID 不能为空");
    if (activeRelease_ && *activeRelease_ == release) {
        return Status::failure("station.already_active", "这个上线包已经是工位当前生效的上线包");
    }
    // S2：最多一个待切换的上线包，新的部署替换旧的
    std::string replaced = pendingRelease_ ? pendingRelease_->releaseId : std::string();
    if (replaced == release) replaced.clear();  // 重复部署同一个：重新加载，不算替换
    pendingRelease_ = PendingRelease{release, false};
    markDirty();
    record(std::make_shared<ReleaseDeployed>(id_, std::move(release), std::move(replaced)));
    return Status::success();
}

Status Station::markPendingReady(const ReleaseId& release) {
    if (!pendingRelease_ || pendingRelease_->releaseId != release) {
        // 后台加载完成时，这次部署可能已经被更新的部署替换了（S2），或者被回滚丢弃了
        return Status::failure("station.pending_mismatch", "上线包 " + release + " 已经不是待切换的上线包");
    }
    if (pendingRelease_->ready) return Status::success();
    pendingRelease_->ready = true;
    markDirty();
    record(std::make_shared<ReleaseReady>(id_, release));
    return Status::success();
}

Status Station::markPendingFailed(const ReleaseId& release, const std::string& reason) {
    if (!pendingRelease_ || pendingRelease_->releaseId != release) {
        return Status::failure("station.pending_mismatch", "上线包 " + release + " 已经不是待切换的上线包");
    }
    pendingRelease_.reset();  // 继续使用当前生效的上线包，产线不受影响（G5）
    markDirty();
    record(std::make_shared<ReleaseLoadFailed>(id_, release, reason));
    return Status::success();
}

void Station::activatePending() {
    std::string from;
    if (activeRelease_) {
        from = *activeRelease_;
        pushHistory(*activeRelease_);
    }
    activeRelease_ = pendingRelease_->releaseId;
    pendingRelease_.reset();
    consecutiveErrors_ = 0;  // P7：统计的是“切换后”连续出现的 ERROR
    markDirty();
    record(std::make_shared<HotSwapped>(id_, std::move(from), *activeRelease_, sequence_ + 1));
}

void Station::doRollback(RollbackCause cause, bool withdrawRequested) {
    std::string from = *activeRelease_;
    activeRelease_ = releaseHistory_.back();
    releaseHistory_.pop_back();
    // 回滚表示“回到之前验证过的状态”。如果还留着一个待切换的上线包，下一次检测就会立刻切换到它，
    // 回滚等于没有做。所以一并丢弃。
    std::string discarded = pendingRelease_ ? pendingRelease_->releaseId : std::string();
    pendingRelease_.reset();
    consecutiveErrors_ = 0;
    markDirty();
    record(std::make_shared<RolledBack>(id_, std::move(from), *activeRelease_, cause, withdrawRequested,
                                        std::move(discarded)));
}

Status Station::rollback(bool withdrawRequested) {
    if (!activeRelease_ || releaseHistory_.empty()) {
        return Status::failure("station.no_rollback_target", "没有可以回滚到的上线包");  // S4
    }
    doRollback(RollbackCause::Manual, withdrawRequested);
    return Status::success();
}

// ---------------- 生命周期 ----------------

Status Station::start() {
    if (state_ == StationState::Running) return Status::failure("station.already_running", "工位已经在运行");
    if (state_ == StationState::Paused) return Status::failure("station.paused", "工位处于暂停状态，请用“继续”");
    // 工位没有在运行，不存在“正在进行的检测”，所以就绪的待切换上线包可以直接生效（S3）
    if (pendingRelease_ && pendingRelease_->ready) activatePending();
    if (!activeRelease_) {
        return Status::failure("station.no_active_release", "工位还没有可用的上线包，请先部署");  // S1
    }
    state_ = StationState::Running;
    consecutiveErrors_ = 0;
    markDirty();
    record(std::make_shared<StationStarted>(id_, *activeRelease_));
    return Status::success();
}

Status Station::pause() {
    if (state_ != StationState::Running) return Status::failure("station.not_running", "工位没有在运行");
    state_ = StationState::Paused;
    markDirty();
    record(std::make_shared<StationPaused>(id_));
    return Status::success();
}

Status Station::resume() {
    if (state_ != StationState::Paused) return Status::failure("station.not_paused", "工位没有处于暂停状态");
    state_ = StationState::Running;
    markDirty();
    record(std::make_shared<StationResumed>(id_));
    return Status::success();
}

Status Station::stop() {
    if (state_ == StationState::Stopped) return Status::failure("station.already_stopped", "工位已经停止");
    state_ = StationState::Stopped;
    markDirty();
    record(std::make_shared<StationStopped>(id_));
    return Status::success();
}

// ---------------- 检测 ----------------

Result<InspectionTicket> Station::beginInspection() {
    using R = Result<InspectionTicket>;
    if (state_ != StationState::Running) {
        return R::failure("station.not_running", std::string("工位没有在运行（当前状态：") + toString(state_) + "）");
    }
    if (inspecting_) return R::failure("station.inspection_in_progress", "上一次检测还没有结束");  // D5-4

    // S3：热切换只发生在这里，也就是两次检测之间。正在进行的检测不会被打断，
    // 因为它手里的入场券已经写明了用哪个上线包（S5）。
    bool swapped = false;
    if (pendingRelease_ && pendingRelease_->ready) {
        activatePending();
        swapped = true;
    }
    inspecting_ = true;
    ++sequence_;
    return InspectionTicket{*activeRelease_, sequence_, swapped};  // S1 保证运行中一定有生效的上线包
}

Status Station::recordOutcome(const InspectionTicket& ticket, Outcome outcome) {
    if (!inspecting_) return Status::failure("station.no_inspection", "没有正在进行的检测");
    inspecting_ = false;

    // 检测进行期间如果发生了手动回滚，这次检测用的已经不是当前生效的上线包，
    // 它的结果不应该算到新的上线包头上。
    if (!activeRelease_ || ticket.releaseId != *activeRelease_) return Status::success();

    if (outcome != Outcome::Error) {
        if (consecutiveErrors_ != 0) {
            consecutiveErrors_ = 0;
            markDirty();
        }
        return Status::success();
    }

    ++consecutiveErrors_;
    markDirty();
    if (consecutiveErrors_ < policy_.autoRollbackAfterErrors) return Status::success();

    if (!releaseHistory_.empty()) {
        doRollback(RollbackCause::Auto, false);  // P7。是否撤回出问题的上线包由人决定
    } else if (state_ == StationState::Running) {
        state_ = StationState::Faulted;
        record(std::make_shared<StationFaulted>(
            id_, "连续 " + std::to_string(consecutiveErrors_) + " 次检测出现 ERROR，并且没有可以回滚到的上线包"));
    }
    return Status::success();
}

}  // namespace vision::station
