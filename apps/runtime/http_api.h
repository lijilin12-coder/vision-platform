#pragma once

#include <memory>
#include <string>

#include "platform.h"

namespace vision::runtime {

// vision-runtime 的 HTTP 接口：一个【驱动适配器】，和命令行是同一层的东西。
//
// 它只做翻译：HTTP 请求 → 调用应用服务 → 把结果写成 JSON。这里没有任何业务规则——
// 连线类型对不对、参数在不在范围里、方案能不能发布，都由应用服务后面的领域层决定，
// 出错时的 code 和 message 原样交给调用方（所以和命令行里看到的一字不差）。
//
// 接口一览见 docs/dev/http-api.md。
//
// 没有登录和权限：只应该监听本机（127.0.0.1）。
class ApiServer {
public:
    explicit ApiServer(platform::Platform& platform);
    ~ApiServer();
    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // 绑定端口。port 为 0 表示让系统挑一个空闲的（测试用）。返回实际的端口；失败返回 -1。
    int bind(const std::string& host, int port);
    // 开始处理请求。一直不返回，直到别的线程调用 stop()。
    void serve();
    void stop();

    // 让服务同时提供界面的文件（web/dist 那个目录）：浏览器打开 http://<地址>/ 就是界面。
    // 目录不存在时返回 false，接口照常可用。要在 serve() 之前调用。
    bool serveWebFiles(const std::string& directory);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vision::runtime
