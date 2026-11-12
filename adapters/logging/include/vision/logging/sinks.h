#pragma once

// 日志接收器：控制台、按大小轮转的文件。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "vision/common/log.h"

namespace vision::logging {

// 写到标准错误输出。只输出不低于 minLevel 的日志。
class ConsoleSink final : public LogSink {
public:
    explicit ConsoleSink(LogLevel minLevel = LogLevel::Trace) : minLevel_(minLevel) {}
    void write(const LogRecord& record) override;
    void flush() override;

private:
    LogLevel minLevel_;
};

// 写到文件，按大小轮转：
//   当前文件 <dir>/<name>.log 超过 maxBytes 时，改名为 <name>.1.log，原来的 .1 变成 .2……
//   最多保留 maxFiles 个旧文件，更旧的删除。所以日志占用的磁盘空间有上限：
//   大约是 maxBytes × (maxFiles + 1)。长期运行的程序必须这样，否则磁盘迟早被写满。
//
// 每写一条就刷新到磁盘：程序崩溃时，崩溃前的最后几条日志最有价值，不能留在缓冲区里丢掉。
class RotatingFileSink final : public LogSink {
public:
    RotatingFileSink(std::filesystem::path dir, std::string name, LogLevel minLevel = LogLevel::Trace,
                     std::uintmax_t maxBytes = 5 * 1024 * 1024, int maxFiles = 5);

    void write(const LogRecord& record) override;
    void flush() override;

    bool ok() const { return file_.is_open(); }
    std::filesystem::path currentPath() const { return pathFor(0); }

private:
    std::filesystem::path pathFor(int index) const;
    void open();
    void rotate();

    std::filesystem::path dir_;
    std::string name_;
    LogLevel minLevel_;
    std::uintmax_t maxBytes_;
    int maxFiles_;
    std::ofstream file_;
    std::uintmax_t size_ = 0;
};

struct LoggingOptions {
    std::filesystem::path dir;             // 日志目录；为空则不写文件
    std::string name = "vision";           // 文件名（不含扩展名）
    LogLevel level = LogLevel::Info;       // 写入文件的最低级别
    LogLevel consoleLevel = LogLevel::Warn;  // 输出到控制台的最低级别；Off 表示不输出到控制台
};

// 清空已有的接收器，按选项重新配置全局日志器。返回日志文件的路径（没有文件时为空）。
std::filesystem::path setupLogging(const LoggingOptions& options);

}  // namespace vision::logging
