#pragma once

// BC-5 工位运行的公开接口（POC-9：启动、部署、热切换、回滚，驱动检测）。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"
#include "vision/execution/api.h"
#include "vision/kernel/image.h"
#include "vision/release/api.h"

namespace vision::station {

class StationRepository;  // 领域层的仓储接口，由组装根传入实现

struct StationOptions {
    int autoRollbackAfterErrors = 3;  // 热切换后连续多少次 ERROR 自动回滚
    int historyDepth = 5;             // 最多记住多少个可以回滚到的上线包
};

struct StationView {
    std::string id;
    std::string name;
    std::string state;  // stopped / running / paused / faulted
    std::string activeRelease;
    std::string pendingRelease;
    bool pendingReady = false;
    std::vector<std::string> releaseHistory;  // 从旧到新
    int consecutiveErrors = 0;
    std::uint64_t sequence = 0;
};

// 一次产线检测的结果
struct StationInspection {
    execution::InspectionReport report;
    std::string releaseId;       // 这次检测用的上线包：检测开始时工位生效的那一个（H-04）
    std::uint64_t sequence = 0;  // 本工位的第几次检测
    bool swapped = false;        // 这次检测开始前是否发生了热切换
    double swapMicros = 0;       // 热切换本身花的时间（没有切换时为 0）
};

// 应用服务。线程模型：
//   - inspect 可以和 deploy、rollback、pause 等命令在不同的线程里同时调用；
//   - 同一个工位的 inspect 一次只进行一个（D5-4），不同工位之间互不影响（H-05）；
//   - deploy 立即返回，上线包在后台线程里加载、预热，完成后在下一次检测开始前切换。
class StationService {
public:
    StationService(StationRepository& stations, release::ReleaseService& releases, execution::ExecutionService& execution,
                   EventPublisher& events);
    ~StationService();  // 等待所有后台加载结束
    StationService(const StationService&) = delete;
    StationService& operator=(const StationService&) = delete;

    Result<std::string> createStation(const std::string& name, const StationOptions& options = {});

    // 部署：检查上线包可以部署，记为待切换，然后在后台加载。立即返回。
    Status deploy(const std::string& stationId, const std::string& releaseId);
    // 等待这个工位的后台加载全部结束（成功或失败）。命令行和测试用。
    Status waitForLoading(const std::string& stationId);

    Status start(const std::string& stationId);   // 热启动：加载上次生效的上线包并预热
    Status pause(const std::string& stationId);
    Status resume(const std::string& stationId);
    Status stop(const std::string& stationId);

    // 回滚到上一个上线包。withdraw 为 true 时同时撤回出问题的上线包（F-5）。
    Status rollback(const std::string& stationId, bool withdraw = false);

    // 对一帧图像做一次产线检测。
    Result<StationInspection> inspect(const std::string& stationId, const Image& frame);

    Result<StationView> getStation(const std::string& stationId);
    std::vector<std::string> listStations();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vision::station
