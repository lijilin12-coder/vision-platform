#include "vision/station/api.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>

#include "vision/common/id.h"
#include "vision/common/log.h"
#include "vision/station/domain/station.h"

namespace vision::station {

namespace {

// 已经加载好、可以直接用来检测的上线包。用 shared_ptr 持有：
// 正在进行的检测手里拿着一份，即使这时工位切换到了别的上线包，它用的内容也不会消失（S5）。
struct LoadedRelease {
    release::ReleaseContent content;
};
using LoadedPtr = std::shared_ptr<const LoadedRelease>;

Outcome toOutcome(const std::string& verdict) {
    if (verdict == "OK") return Outcome::Ok;
    if (verdict == "NG") return Outcome::Ng;
    return Outcome::Error;
}

StationView toView(const Station& s) {
    StationView v;
    v.id = s.id();
    v.name = s.name();
    v.state = toString(s.state());
    v.activeRelease = s.activeRelease().value_or("");
    if (s.pendingRelease()) {
        v.pendingRelease = s.pendingRelease()->releaseId;
        v.pendingReady = s.pendingRelease()->ready;
    }
    v.releaseHistory = s.releaseHistory();
    v.consecutiveErrors = s.consecutiveErrors();
    v.sequence = s.sequence();
    if (s.imageSource()) {
        v.imageSourceKind = s.imageSource()->kind;
        v.imageSourceLocation = s.imageSource()->location;
    }
    return v;
}

}  // namespace

struct StationService::Impl {
    // 一个工位在内存里的全部状态
    struct Entry {
        explicit Entry(Station s) : station(std::move(s)) {}
        std::mutex stateMutex;    // 保护 station、loaded、loadsInFlight。只在很短的时间里持有
        std::mutex inspectMutex;  // 让同一个工位的检测一次只进行一个（D5-4）
        std::condition_variable loadingChanged;
        Station station;
        std::map<ReleaseId, LoadedPtr> loaded;
        int loadsInFlight = 0;
        // 打开着的图像源。只在持有 inspectMutex 时使用（取图和检测是同一件事的两步）
        std::unique_ptr<ImageSource> source;
        bool closeSource = false;  // 工位停止过：下一次取图前要重新打开。由 stateMutex 保护
    };
    using EntryPtr = std::shared_ptr<Entry>;

    StationRepository& repository;
    release::ReleaseService& releases;
    execution::ExecutionService& execution;
    EventPublisher& events;

    std::mutex entriesMutex;
    std::map<StationId, EntryPtr> entries;
    std::mutex repositoryMutex;  // 仓储的实现不要求线程安全
    std::mutex loadersMutex;
    std::vector<std::future<void>> loaders;
    ImageSourceFactory* imageSources = nullptr;

    Impl(StationRepository& r, release::ReleaseService& rel, execution::ExecutionService& ex, EventPublisher& ev)
        : repository(r), releases(rel), execution(ex), events(ev) {}

    // ---- 工位的取得、保存 ----

    Result<EntryPtr> entry(const std::string& stationId) {
        EntryPtr e;
        {
            std::lock_guard<std::mutex> lock(entriesMutex);
            if (auto it = entries.find(stationId); it != entries.end()) return it->second;
            std::optional<Station> station;
            {
                std::lock_guard<std::mutex> repoLock(repositoryMutex);
                station = repository.find(stationId);
            }
            if (!station) return Result<EntryPtr>::failure("station.not_found", "找不到工位 " + stationId);
            // 这是本进程第一次用到这个工位：上次保存的“运行中”“已就绪”不再成立
            station->recoverAfterRestart();
            e = std::make_shared<Entry>(std::move(*station));
            entries[stationId] = e;
        }
        std::string pendingToReload;
        {
            std::lock_guard<std::mutex> lock(e->stateMutex);
            (void)persist(*e);
            if (e->station.pendingRelease()) pendingToReload = e->station.pendingRelease()->releaseId;
        }
        // 重启前还没来得及切换的部署，重新在后台加载
        if (!pendingToReload.empty()) loadInBackground(e, pendingToReload);
        return e;
    }

    // 需要时保存，并取出待发布的事件。调用时必须持有 e.stateMutex。
    // 事件要在放开锁之后再发布：事件处理器里可能会回头调用本服务。
    std::vector<EventPtr> persist(Entry& e) {
        if (e.station.takeDirty()) {
            std::lock_guard<std::mutex> repoLock(repositoryMutex);
            if (auto st = repository.save(e.station); !st) {
                log::error("station", "保存工位失败", {{"station", e.station.id()}, {"reason", st.error().message}});
            }
        }
        return e.station.pullEvents();
    }

    void publish(const std::vector<EventPtr>& pulled) {
        for (const auto& ev : pulled) {
            events.publish(ev);
            // 回滚时要求撤回出问题的上线包：撤回修改的是另一个聚合（Release），所以在这里由应用层去做
            if (const auto* rb = dynamic_cast<const RolledBack*>(ev.get()); rb != nullptr && rb->withdrawRequested) {
                if (auto st = releases.withdraw(rb->fromRelease); !st) {
                    log::warn("station", "回滚后撤回上线包失败",
                              {{"release", rb->fromRelease}, {"reason", st.error().message}});
                }
            }
        }
    }

    // ---- 上线包的加载 ----

    // 取得上线包的内容并预热。比较慢（读文件、编译执行计划，将来还要加载模型），不要在持有锁时调用。
    Result<LoadedPtr> load(const ReleaseId& releaseId) {
        auto content = releases.getContent(releaseId);
        if (!content) return content.error();
        if (auto st = execution.prepare(content.value().recipe); !st) return st.error();
        return LoadedPtr(std::make_shared<LoadedRelease>(LoadedRelease{std::move(content).value()}));
    }

    void loadInBackground(const EntryPtr& e, const ReleaseId& releaseId) {
        {
            std::lock_guard<std::mutex> lock(e->stateMutex);
            ++e->loadsInFlight;
        }
        auto task = std::async(std::launch::async, [this, e, releaseId] {
            const auto t0 = std::chrono::steady_clock::now();
            auto loaded = load(releaseId);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::vector<EventPtr> pulled;
            {
                std::lock_guard<std::mutex> lock(e->stateMutex);
                if (loaded) {
                    // 加载期间，这次部署可能已经被更新的部署替换，或者被回滚丢弃了：那就什么都不做
                    if (e->station.markPendingReady(releaseId)) e->loaded[releaseId] = loaded.value();
                    log::info("station", "上线包已加载", {{"station", e->station.id()}, {"release", releaseId}, {"ms", std::to_string(ms)}});
                } else {
                    (void)e->station.markPendingFailed(releaseId, loaded.error().message);
                    log::error("station", "上线包加载失败",
                               {{"station", e->station.id()}, {"release", releaseId}, {"reason", loaded.error().message}});
                }
                pulled = persist(*e);
            }
            // 顺序很重要：先发布事件，再宣布“加载结束”。
            // 反过来的话，waitForLoading() 的调用者可能在“上线包已就绪 / 加载失败”事件发布之前就被唤醒，
            // 看到的是“加载结束了，却没有任何事件”。（事件要在锁外发布：订阅者可能回头调用本服务。）
            publish(pulled);
            {
                std::lock_guard<std::mutex> lock(e->stateMutex);
                --e->loadsInFlight;
            }
            e->loadingChanged.notify_all();
        });
        std::lock_guard<std::mutex> lock(loadersMutex);
        // 顺手清理已经结束的后台任务
        loaders.erase(std::remove_if(loaders.begin(), loaders.end(),
                                     [](std::future<void>& f) {
                                         return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                                     }),
                      loaders.end());
        loaders.push_back(std::move(task));
    }

    // 只保留还可能用到的上线包：生效的、待切换的、历史里的。调用时必须持有 e.stateMutex。
    static void evictUnused(Entry& e) {
        for (auto it = e.loaded.begin(); it != e.loaded.end();) {
            const ReleaseId& id = it->first;
            const auto& s = e.station;
            const bool needed = (s.activeRelease() && *s.activeRelease() == id) ||
                                (s.pendingRelease() && s.pendingRelease()->releaseId == id) ||
                                std::find(s.releaseHistory().begin(), s.releaseHistory().end(), id) != s.releaseHistory().end();
            it = needed ? std::next(it) : e.loaded.erase(it);
        }
    }

    // 执行一个只修改工位状态的简单命令
    template <typename Fn>
    Status command(const std::string& stationId, Fn&& fn) {
        auto e = entry(stationId);
        if (!e) return e.error();
        std::vector<EventPtr> pulled;
        Status result = Status::success();
        {
            std::lock_guard<std::mutex> lock(e.value()->stateMutex);
            result = fn(e.value()->station);
            evictUnused(*e.value());
            pulled = persist(*e.value());
        }
        publish(pulled);
        return result;
    }
};

StationService::StationService(StationRepository& stations, release::ReleaseService& releases,
                               execution::ExecutionService& execution, EventPublisher& events)
    : impl_(std::make_unique<Impl>(stations, releases, execution, events)) {}

StationService::~StationService() { finishBackgroundWork(); }

void StationService::finishBackgroundWork() {
    std::vector<std::future<void>> pending;
    {
        std::lock_guard<std::mutex> lock(impl_->loadersMutex);
        pending.swap(impl_->loaders);
    }
    for (auto& f : pending) f.wait();
}

Result<std::string> StationService::createStation(const std::string& name, const StationOptions& options) {
    auto station = Station::create(newUuid(), name, StationPolicy{options.autoRollbackAfterErrors, options.historyDepth});
    if (!station) return station.error();
    const std::string id = station.value().id();
    auto e = std::make_shared<Impl::Entry>(std::move(station).value());
    std::vector<EventPtr> pulled;
    {
        std::lock_guard<std::mutex> lock(e->stateMutex);
        pulled = impl_->persist(*e);
    }
    {
        std::lock_guard<std::mutex> lock(impl_->entriesMutex);
        impl_->entries[id] = e;
    }
    impl_->publish(pulled);
    return id;
}

Status StationService::deploy(const std::string& stationId, const std::string& releaseId) {
    auto e = impl_->entry(stationId);
    if (!e) return e.error();

    // 部署策略（DeploymentPolicy）：上线包必须存在，并且没有被撤回（L6）。
    // 验收检查和跨平台一致性检查（L4）实现之后，也在这里检查。
    auto manifest = impl_->releases.getManifest(releaseId);
    if (!manifest) return manifest.error();
    if (manifest.value().withdrawn) {
        return Status::failure("station.release_withdrawn", "上线包 " + releaseId + " 已经被撤回，不能部署");
    }

    std::vector<EventPtr> pulled;
    {
        std::lock_guard<std::mutex> lock(e.value()->stateMutex);
        if (auto st = e.value()->station.deploy(releaseId); !st) return st;
        pulled = impl_->persist(*e.value());
    }
    impl_->publish(pulled);
    impl_->loadInBackground(e.value(), releaseId);  // 不等它完成：产线继续用当前的上线包检测
    return Status::success();
}

Status StationService::waitForLoading(const std::string& stationId) {
    auto e = impl_->entry(stationId);
    if (!e) return e.error();
    std::unique_lock<std::mutex> lock(e.value()->stateMutex);
    e.value()->loadingChanged.wait(lock, [&] { return e.value()->loadsInFlight == 0; });
    return Status::success();
}

Status StationService::start(const std::string& stationId) {
    auto e = impl_->entry(stationId);
    if (!e) return e.error();
    auto& entry = *e.value();

    // 启动后要用的上线包：已就绪的待切换上线包会在启动时生效，否则是上次生效的那个
    ReleaseId target;
    {
        std::lock_guard<std::mutex> lock(entry.stateMutex);
        const auto& s = entry.station;
        if (s.pendingRelease() && s.pendingRelease()->ready) {
            target = s.pendingRelease()->releaseId;
        } else if (s.activeRelease()) {
            target = *s.activeRelease();
        }
        if (!target.empty() && entry.loaded.count(target) != 0) target.clear();  // 已经加载好了
    }
    // 热启动：先把上线包加载、预热好，再开始接受检测（P8）
    LoadedPtr loaded;
    if (!target.empty()) {
        auto r = impl_->load(target);
        if (!r) return r.error();
        loaded = r.value();
    }

    std::vector<EventPtr> pulled;
    Status result = Status::success();
    {
        std::lock_guard<std::mutex> lock(entry.stateMutex);
        if (loaded) entry.loaded[target] = loaded;
        result = entry.station.start();
        Impl::evictUnused(entry);
        pulled = impl_->persist(entry);
    }
    impl_->publish(pulled);
    return result;
}

Status StationService::pause(const std::string& stationId) {
    return impl_->command(stationId, [](Station& s) { return s.pause(); });
}

Status StationService::resume(const std::string& stationId) {
    return impl_->command(stationId, [](Station& s) { return s.resume(); });
}

Status StationService::stop(const std::string& stationId) {
    auto e = impl_->entry(stationId);
    if (!e) return e.error();
    const Status st = impl_->command(stationId, [](Station& s) { return s.stop(); });
    if (st) {
        // 图像源不能在这里直接关：可能有一次检测正拿着它。记下来，下一次取图前重新打开
        std::lock_guard<std::mutex> lock(e.value()->stateMutex);
        e.value()->closeSource = true;
    }
    return st;
}

Status StationService::rollback(const std::string& stationId, bool withdraw) {
    return impl_->command(stationId, [withdraw](Station& s) { return s.rollback(withdraw); });
}

Result<StationInspection> StationService::inspect(const std::string& stationId, const Image& frame) {
    using R = Result<StationInspection>;
    auto e = impl_->entry(stationId);
    if (!e) return e.error();
    auto& entry = *e.value();

    std::lock_guard<std::mutex> oneAtATime(entry.inspectMutex);

    // ---- 第一步：开始检测。必要时热切换；确定这次检测用哪个上线包 ----
    InspectionTicket ticket;
    LoadedPtr loaded;
    double swapMicros = 0;
    std::vector<EventPtr> pulled;
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(entry.stateMutex);
        auto began = entry.station.beginInspection();
        if (!began) return began.error();
        ticket = began.value();
        if (auto it = entry.loaded.find(ticket.releaseId); it != entry.loaded.end()) loaded = it->second;
        if (ticket.swapped) Impl::evictUnused(entry);
        pulled = impl_->persist(entry);
        if (ticket.swapped) {
            swapMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        }
    }
    impl_->publish(pulled);

    // 正常情况下生效的上线包一定已经加载好了。只有程序重启之后回滚到一个本次运行还没用过的上线包时，
    // 才需要在这里现加载一次。
    if (!loaded) {
        log::warn("station", "检测时才加载上线包", {{"station", stationId}, {"release", ticket.releaseId}});
        if (auto r = impl_->load(ticket.releaseId)) {
            loaded = r.value();
            std::lock_guard<std::mutex> lock(entry.stateMutex);
            entry.loaded[ticket.releaseId] = loaded;
            Impl::evictUnused(entry);
        }
    }

    // ---- 第二步：执行检测。不持有工位的锁，所以这期间可以部署、回滚 ----
    Outcome outcome = Outcome::Error;
    R result = R::failure("station.release_not_loaded", "上线包 " + ticket.releaseId + " 无法加载");
    if (loaded) {
        auto report = impl_->execution.inspect(loaded->content.recipe, frame, execution::RunPurpose::Production);
        if (report) {
            outcome = toOutcome(report.value().verdict);
            result = StationInspection{std::move(report).value(), ticket.releaseId, ticket.sequence, ticket.swapped, swapMicros, {}, {}};
        } else {
            result = report.error();
        }
    }

    // ---- 第三步：记录结果。连续 ERROR 达到上限时自动回滚 ----
    {
        std::lock_guard<std::mutex> lock(entry.stateMutex);
        (void)entry.station.recordOutcome(ticket, outcome);
        Impl::evictUnused(entry);
        pulled = impl_->persist(entry);
    }
    impl_->publish(pulled);
    return result;
}

// ---------------- 图像源 ----------------

void StationService::useImageSources(ImageSourceFactory& factory) { impl_->imageSources = &factory; }

Status StationService::bindImageSource(const std::string& stationId, const std::string& kind, const std::string& location) {
    if (impl_->imageSources == nullptr) return Status::failure("station.no_image_sources", "这个程序没有配置图像源");
    // 先试着打开一次：打不开的（目录不存在、种类不认识）现在就拒绝，而不是等到工位启动之后
    if (auto opened = impl_->imageSources->open(kind, location); !opened) return opened.error();
    auto e = impl_->entry(stationId);
    if (!e) return e.error();
    const Status st = impl_->command(stationId, [&](Station& s) { return s.bindImageSource({kind, location}); });
    if (st) {
        std::lock_guard<std::mutex> lock(e.value()->stateMutex);
        e.value()->closeSource = true;  // 换了图像源：原来打开着的那个不能再用
    }
    return st;
}

Status StationService::unbindImageSource(const std::string& stationId) {
    auto e = impl_->entry(stationId);
    if (!e) return e.error();
    const Status st = impl_->command(stationId, [](Station& s) { return s.unbindImageSource(); });
    if (st) {
        std::lock_guard<std::mutex> lock(e.value()->stateMutex);
        e.value()->closeSource = true;
    }
    return st;
}

Result<std::optional<StationInspection>> StationService::inspectNext(const std::string& stationId) {
    using R = Result<std::optional<StationInspection>>;
    auto e = impl_->entry(stationId);
    if (!e) return e.error();
    auto& entry = *e.value();

    Frame frame;
    {
        // 取图也在“一次只进行一个”的范围里：两个线程同时取，会各拿到一帧，顺序就乱了。
        // 取完就放开；接下来的 inspect 会自己再拿这把锁。
        std::lock_guard<std::mutex> inspecting(entry.inspectMutex);
        std::optional<ImageSourceBinding> binding;
        {
            std::lock_guard<std::mutex> lock(entry.stateMutex);
            if (entry.station.state() != StationState::Running) {
                return R::failure("station.not_running",
                                  std::string("工位没有在运行（当前状态：") + toString(entry.station.state()) + "）");
            }
            binding = entry.station.imageSource();
            if (entry.closeSource) {
                entry.source.reset();
                entry.closeSource = false;
            }
        }
        if (!binding) return R::failure("station.no_image_source", "工位还没有绑定图像源");
        if (!entry.source) {
            if (impl_->imageSources == nullptr) return R::failure("station.no_image_sources", "这个程序没有配置图像源");
            auto opened = impl_->imageSources->open(binding->kind, binding->location);
            if (!opened) return opened.error();
            entry.source = std::move(opened).value();
        }
        auto next = entry.source->next();
        if (!next) return next.error();
        if (!next.value()) return std::optional<StationInspection>{};  // 现在没有新的图像
        frame = std::move(*next.value());
    }
    auto inspected = inspect(stationId, frame.image);
    if (!inspected) return inspected.error();
    inspected.value().frameName = std::move(frame.name);
    inspected.value().frame = std::move(frame.image);
    return std::optional<StationInspection>{std::move(inspected).value()};
}

Result<StationView> StationService::getStation(const std::string& stationId) {
    auto e = impl_->entry(stationId);
    if (!e) return e.error();
    std::lock_guard<std::mutex> lock(e.value()->stateMutex);
    return toView(e.value()->station);
}

std::vector<std::string> StationService::listStations() {
    std::lock_guard<std::mutex> lock(impl_->repositoryMutex);
    return impl_->repository.listIds();
}

}  // namespace vision::station
