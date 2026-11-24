#pragma once

#include <string>
#include <vector>

#include "vision/common/result.h"

namespace vision::http {

struct HttpRequest {
    std::string method = "POST";  // POST、GET、DELETE
    std::string url;
    std::vector<std::string> headers;  // 每一项是 "名字: 值"
    std::string body;             // GET 和 DELETE 不带请求体
    int timeoutSeconds = 120;
};

struct HttpResponse {
    int status = 0;  // HTTP 状态码
    std::string body;
};

// 端口：发一个 HTTP(S) 请求。
// 返回失败表示请求没有发出去或没有收到回复（断网、超时）；收到了回复就返回成功，哪怕状态码是 500。
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual Result<HttpResponse> send(const HttpRequest& request) = 0;
};

// 用系统自带的 curl 命令发请求（macOS、Linux、Windows 10 以上都自带）。
//
// 为什么不直接链接一个 HTTP 库：POC 阶段不想为了一个接口多一个要在三个平台上编译的依赖。
// 鸿蒙上没有 curl 命令，到时候换成系统的网络接口——只需要另写一个 HttpClient 的实现。
//
// 密钥放在请求头里。请求头写在一个只有当前用户能读的临时文件里交给 curl，用完立即删除；
// 不放在命令行参数里（命令行参数能被同一台机器上的其他用户看到）。
//
// 发往本机（localhost、127.0.0.1）的请求不经过代理：训练服务就在本机时，系统配置的代理到不了它。
class CurlCommandHttpClient final : public HttpClient {
public:
    Result<HttpResponse> send(const HttpRequest& request) override;
};

}  // namespace vision::http
