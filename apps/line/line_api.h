#pragma once

#include <memory>
#include <string>

#include "line_platform.h"

namespace vision::line {

// vision-line serve 运行期间的控制接口（HTTP，只监听本机）。
//
// 为什么需要：工位的状态在【正在运行的那个进程】的内存里。另开一个终端执行 vision-line station deploy，
// 那是另一个进程，它只能改磁盘上的文件，正在运行的工位看不到——热切换就不会发生。
// 所以运行期间对工位的操作必须交给正在运行的进程来做：通过这个接口。
//
//   GET  /api/health
//   GET  /api/stations                      所有工位的当前状态
//   GET  /api/stations/{id}
//   POST /api/stations/{id}/deploy          {"releaseId": "..."}    部署；在下一次检测开始前热切换
//   POST /api/stations/{id}/rollback        {"withdraw": false}
//
// 和 vision-runtime 的接口层一样：只做翻译，规则都在应用服务后面；出错时 code 和 message 原样带出。
class ControlApi {
public:
    explicit ControlApi(platform::LinePlatform& platform);
    ~ControlApi();
    ControlApi(const ControlApi&) = delete;
    ControlApi& operator=(const ControlApi&) = delete;

    int bind(const std::string& host, int port);  // port 为 0：让系统挑一个。返回实际的端口，失败返回 -1
    void serve();                                  // 一直不返回，直到 stop()
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- 转发：命令行发现服务正在运行时，把对工位的操作交给它 ----

// 服务运行时在数据目录里留一个记号文件，里面是控制接口的端口。
// 记号文件在、而且那个端口上确实有服务在应答，才算“正在运行”（程序崩溃留下的旧文件不算）。
void writeServeMarker(const std::filesystem::path& dataDir, int port);
void removeServeMarker(const std::filesystem::path& dataDir);
// 返回正在运行的服务的控制地址（例如 "http://127.0.0.1:8760"）；没有在运行时返回空，并顺手清掉旧的记号文件。
std::string runningService(const std::filesystem::path& dataDir);

// 把一个 station 子命令转发给正在运行的服务。返回退出码；这个子命令不支持转发时返回 -1（调用者据此拒绝）。
int forwardStationCommand(const std::string& serviceUrl, const std::vector<std::string>& args);

}  // namespace vision::line
