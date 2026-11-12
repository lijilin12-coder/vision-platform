#include "vision/common/log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace vision {

const char* toString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF";
    }
    return "?";
}

LogLevel parseLogLevel(const std::string& text, LogLevel fallback) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "trace") return LogLevel::Trace;
    if (lower == "debug") return LogLevel::Debug;
    if (lower == "info") return LogLevel::Info;
    if (lower == "warn" || lower == "warning") return LogLevel::Warn;
    if (lower == "error") return LogLevel::Error;
    if (lower == "off") return LogLevel::Off;
    return fallback;
}

Logger& Logger::global() {
    static Logger instance;
    return instance;
}

void Logger::setLevel(LogLevel level) { level_.store(level, std::memory_order_relaxed); }

LogLevel Logger::level() const { return level_.load(std::memory_order_relaxed); }

bool Logger::enabled(LogLevel level) const {
    if (level == LogLevel::Off || level < level_.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return !sinks_.empty();
}

void Logger::addSink(std::shared_ptr<LogSink> sink) {
    if (!sink) return;
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.push_back(std::move(sink));
}

void Logger::clearSinks() {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks_.clear();
}

void Logger::write(LogLevel level, std::string_view component, std::string_view message,
                   std::initializer_list<LogField> fields) {
    // 快速路径：级别不够就立刻返回。到这里为止没有加锁，也没有申请任何内存。
    if (level == LogLevel::Off || level < level_.load(std::memory_order_relaxed)) return;

    LogRecord record;
    record.time = std::chrono::system_clock::now();
    record.level = level;
    record.component = std::string(component);
    record.message = std::string(message);
    record.fields.reserve(fields.size());
    for (const LogField& field : fields) record.fields.emplace_back(std::string(field.key), std::string(field.value));

    // 在锁里调用接收器：保证多个线程写的日志不会互相穿插，也保证顺序和时间一致。
    // 代价是写日志的线程会互相等待，所以接收器必须很快（写一行文本）。
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& sink : sinks_) sink->write(record);
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& sink : sinks_) sink->flush();
}

namespace {

bool needsQuoting(const std::string& value) {
    if (value.empty()) return true;
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c == ' ' || c == '"' || c == '=' || c == '\\' || c < 0x20;
    });
}

void appendEscaped(std::string& out, const std::string& text, bool quoteContext) {
    for (const char c : text) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\\':
                out += quoteContext ? "\\\\" : "\\";
                break;
            case '"':
                out += quoteContext ? "\\\"" : "\"";
                break;
            default: out += c;
        }
    }
}

}  // namespace

std::string formatLogLine(const LogRecord& record) {
    using namespace std::chrono;
    const auto sinceEpoch = record.time.time_since_epoch();
    const std::time_t seconds = static_cast<std::time_t>(duration_cast<std::chrono::seconds>(sinceEpoch).count());
    const int millis = static_cast<int>(duration_cast<milliseconds>(sinceEpoch).count() % 1000);

    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char stamp[64];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                  utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, millis);

    char level[8];
    std::snprintf(level, sizeof(level), "%-5s", toString(record.level));

    std::string line;
    line.reserve(96 + record.message.size());
    line += stamp;
    line += ' ';
    line += level;
    line += " [";
    line += record.component;
    line += "] ";
    appendEscaped(line, record.message, false);  // 消息里的换行也要转义：一条日志必须是一行
    for (const auto& [key, value] : record.fields) {
        line += ' ';
        line += key;
        line += '=';
        if (needsQuoting(value)) {
            line += '"';
            appendEscaped(line, value, true);
            line += '"';
        } else {
            line += value;
        }
    }
    return line;
}

}  // namespace vision
