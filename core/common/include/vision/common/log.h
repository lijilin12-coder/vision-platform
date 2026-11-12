#pragma once

// 日志。
//
// 用法：
//     vision::log::info("station", "热切换完成", {{"from", "R-1"}, {"to", "R-2"}});
//     vision::log::error("imaging", "无法读取图像", {{"path", path}});
//
// 约定：
//   - 第一个参数是“组件”，表示日志来自哪里，例如 "station"、"execution"、"cli"。
//   - 第二个参数是固定的一句话；会变化的数据放进第三个参数的键值对里，方便以后按字段搜索。
//   - 领域层不写日志：它不依赖任何技术。日志由应用层和适配器来写，或者由订阅领域事件的代码来写。
//   - 不要在处理像素的循环里写日志。日志是给“每次检测发生了什么”这个粒度用的。
//
// 这里只定义接口和分发。日志写到哪里（控制台、文件、系统日志）由接收器 LogSink 决定，
// 实现在 adapters/logging 里；鸿蒙上可以换成写入系统日志的接收器。

#include <chrono>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace vision {

// 注意：不能用 ERROR、DEBUG 这样的全大写名字，Windows 的系统头文件把它们定义成了宏。
enum class LogLevel { Trace, Debug, Info, Warn, Error, Off };

const char* toString(LogLevel level);
// 解析 "debug"、"info" 等（不区分大小写）。无法识别时返回 fallback。
LogLevel parseLogLevel(const std::string& text, LogLevel fallback);

using LogFields = std::vector<std::pair<std::string, std::string>>;

struct LogRecord {
    std::chrono::system_clock::time_point time;
    LogLevel level = LogLevel::Info;
    std::string component;
    std::string message;
    LogFields fields;
};

// 接收器：决定一条日志最终写到哪里。实现必须自己保证可以被多个线程调用，
// 不过 Logger 已经保证同一时刻只有一个线程在调用同一个 Logger 的接收器。
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

class Logger {
public:
    // 进程内唯一的日志器。
    static Logger& global();

    void setLevel(LogLevel level);
    LogLevel level() const;
    bool enabled(LogLevel level) const;

    void addSink(std::shared_ptr<LogSink> sink);
    void clearSinks();

    void write(LogLevel level, std::string component, std::string message, LogFields fields = {});
    void flush();

private:
    mutable std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
    std::vector<std::shared_ptr<LogSink>> sinks_;
};

// 把一条记录格式化成一行文本（不含换行）：
//   2026-11-12T10:50:03.880Z INFO  [station] 热切换完成 from=R-1 to=R-2
// 值里有空格、引号或换行时会加上引号并转义，保证一条日志始终是一行。
std::string formatLogLine(const LogRecord& record);

namespace log {

inline void trace(std::string component, std::string message, LogFields fields = {}) {
    Logger::global().write(LogLevel::Trace, std::move(component), std::move(message), std::move(fields));
}
inline void debug(std::string component, std::string message, LogFields fields = {}) {
    Logger::global().write(LogLevel::Debug, std::move(component), std::move(message), std::move(fields));
}
inline void info(std::string component, std::string message, LogFields fields = {}) {
    Logger::global().write(LogLevel::Info, std::move(component), std::move(message), std::move(fields));
}
inline void warn(std::string component, std::string message, LogFields fields = {}) {
    Logger::global().write(LogLevel::Warn, std::move(component), std::move(message), std::move(fields));
}
inline void error(std::string component, std::string message, LogFields fields = {}) {
    Logger::global().write(LogLevel::Error, std::move(component), std::move(message), std::move(fields));
}

}  // namespace log
}  // namespace vision
