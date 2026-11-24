#pragma once

// BC-5 工位运行的公开接口（POC-9：启动、部署、热切换、回滚，驱动检测）。

#include <cstdint>
#include <memory>
#include <optional>
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
    std::string imageSourceKind, imageSourceLocation;  // 没有绑定图像源时为空
};

// ===== 端口：图像源 =====
// 工位的图像从哪里来。现在只有“一个目录里的图像”这一种实现；相机以后是另一种实现，工位这边不用改。

struct Frame {
    Image image;
    std::string name;  // 这一帧的名字，给人看的（对目录来说是文件名）
};

class ImageSource {
public:
    virtual ~ImageSource() = default;
    // 取下一帧。
    //   有图像        → 返回它
    //   现在没有新的   → 返回空（不是出错：目录里的图像都处理过了，或者相机还没有拍下一张）
    //   这一帧坏了     → 返回失败（文件损坏、相机出错）。再调用会接着取后面的，不会卡在坏的那一帧上
    virtual Result<std::optional<Frame>> next() = 0;
};

class ImageSourceFactory {
public:
    virtual ~ImageSourceFactory() = default;
    // 按“种类 + 位置”打开一个图像源。种类不认识、位置不存在时返回失败。
    virtual Result<std::unique_ptr<ImageSource>> open(const std::string& kind, const std::string& location) = 0;
};

// 一次产线检测的结果
struct StationInspection {
    execution::InspectionReport report;
    std::string releaseId;       // 这次检测用的上线包：检测开始时工位生效的那一个（H-04）
    std::uint64_t sequence = 0;  // 本工位的第几次检测
    bool swapped = false;        // 这次检测开始前是否发生了热切换
    double swapMicros = 0;       // 热切换本身花的时间（没有切换时为 0）
    std::string frameName;       // 图像来自图像源时：这一帧的名字
    Image frame;                 // 图像来自图像源时：这一帧的图像（和检测用的是同一份像素，不是复制品）
};

// ===== 端口：有一次产线检测完成了 =====
// 想知道“工位检测了什么”的一方实现它（现在是质量分析：把每次检测记下来）。
// 在做检测的那个线程里调用，检测的结果已经确定之后。实现要自己保证够快、不抛异常。
class InspectionObserver {
public:
    virtual ~InspectionObserver() = default;
    virtual void onInspected(const std::string& stationId, const StationInspection& inspection) = 0;
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
    // 等待所有工位的后台加载结束。组装根在开始销毁其他服务【之前】要调用它：
    // 后台加载会用到检测引擎、模板、模型，它们不能在加载还没结束时就被销毁。
    void finishBackgroundWork();
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

    // 对一帧图像做一次产线检测。frameName：这一帧的名字（给人看的，会进检测记录），可以为空
    Result<StationInspection> inspect(const std::string& stationId, const Image& frame, const std::string& frameName = {});
    // 每完成一次产线检测通知一声。组装根传入；不设置就没有人被通知
    void useInspectionObserver(InspectionObserver& observer);

    // ---- 图像源（STA-01）----
    // 用到图像源的功能需要它；组装根传入。
    void useImageSources(ImageSourceFactory& factory);
    // 给工位绑定图像源。只能在工位停止时做（S7）；位置不存在、种类不认识时被拒绝，不会留下一个打不开的绑定。
    Status bindImageSource(const std::string& stationId, const std::string& kind, const std::string& location);
    Status unbindImageSource(const std::string& stationId);
    // 从工位绑定的图像源取下一帧并检测。图像源现在没有新的图像时返回空。
    // 图像源在工位启动后第一次取图时打开，工位停止时关闭——所以停止再启动，目录里的图像会从头再来一遍。
    Result<std::optional<StationInspection>> inspectNext(const std::string& stationId);

    Result<StationView> getStation(const std::string& stationId);
    std::vector<std::string> listStations();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vision::station
