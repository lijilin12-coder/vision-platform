#include "vision/http/http_client.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "vision/common/id.h"
#include "vision/common/path.h"

#ifdef _WIN32
#define VISION_POPEN _popen
#define VISION_PCLOSE _pclose
#else
#define VISION_POPEN popen
#define VISION_PCLOSE pclose
#endif

namespace vision::http {

namespace fs = std::filesystem;

namespace {

// curl 配置文件里，值写在双引号中，反斜杠和双引号要转义
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n' || c == '\r') continue;  // 请求头里不允许换行
        out += c;
    }
    return out + "\"";
}

// 离开作用域时删除临时文件（里面有密钥）
struct TempFiles {
    std::vector<fs::path> paths;
    ~TempFiles() {
        std::error_code ec;
        for (const auto& p : paths) fs::remove(p, ec);
    }
};

}  // namespace

Result<HttpResponse> CurlCommandHttpClient::send(const HttpRequest& request) {
    using R = Result<HttpResponse>;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec);
    if (ec) return R::failure("http.temp", "找不到临时目录");
    const std::string stem = "vision-http-" + newUuid();
    TempFiles temp;
    const fs::path configFile = dir / (stem + ".cfg"), bodyFile = dir / (stem + ".body"), outFile = dir / (stem + ".out");
    temp.paths = {configFile, bodyFile, outFile};

    const bool hasBody = request.method == "POST" || !request.body.empty();
    if (hasBody) {
        std::ofstream body(bodyFile, std::ios::binary);
        body << request.body;
        if (!body) return R::failure("http.temp", "无法写临时文件");
    }
    {
        // 先建一个空文件并收紧权限，再往里写密钥
        { std::ofstream create(configFile, std::ios::binary); }
        fs::permissions(configFile, fs::perms::owner_read | fs::perms::owner_write, ec);
        std::ofstream config(configFile, std::ios::binary);
        config << "url = " << quoted(request.url) << "\n";
        config << "request = " << quoted(request.method) << "\n";
        config << "noproxy = \"localhost,127.0.0.1\"\n";
        for (const auto& h : request.headers) config << "header = " << quoted(h) << "\n";
        if (hasBody) config << "data-binary = " << quoted("@" + pathToUtf8(bodyFile)) << "\n";
        config << "output = " << quoted(pathToUtf8(outFile)) << "\n";
        config << "write-out = \"%{http_code}\"\n";
        config << "max-time = " << request.timeoutSeconds << "\n";
        config << "silent\nshow-error\n";
        if (!config) return R::failure("http.temp", "无法写临时文件");
    }

    // 命令行上只有配置文件的路径。2>&1：把 curl 的出错信息也读回来
    const std::string command = "curl --config \"" + pathToUtf8(configFile) + "\" 2>&1";
    FILE* pipe = VISION_POPEN(command.c_str(), "r");
    if (pipe == nullptr) return R::failure("http.no_curl", "无法运行 curl 命令");
    std::string printed;
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) printed += buffer;
    const int exitCode = VISION_PCLOSE(pipe);

    if (exitCode != 0) {
        // 没有收到回复：断网、域名解析失败、超时、没有安装 curl……
        // 没有收到回复时 curl 仍然会按 write-out 打印一个 "000"：去掉它，只留出错信息
        if (printed.size() >= 3 && printed.compare(printed.size() - 3, 3, "000") == 0) printed.erase(printed.size() - 3);
        while (!printed.empty() && (printed.back() == '\n' || printed.back() == '\r')) printed.pop_back();
        return R::failure("http.unreachable", "请求没有成功：" + (printed.empty() ? "curl 退出码 " + std::to_string(exitCode) : printed));
    }
    HttpResponse response;
    try {
        response.status = std::stoi(printed.substr(printed.size() >= 3 ? printed.size() - 3 : 0));
    } catch (const std::exception&) {
        return R::failure("http.unreachable", "看不懂 curl 的输出：" + printed);
    }
    std::ifstream out(outFile, std::ios::binary);
    std::stringstream ss;
    ss << out.rdbuf();
    response.body = ss.str();
    return response;
}

}  // namespace vision::http
