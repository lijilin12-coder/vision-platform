#include "vision/logging/sinks.h"

#include <algorithm>
#include <iostream>
#include <system_error>

namespace vision::logging {

namespace fs = std::filesystem;

void ConsoleSink::write(const LogRecord& record) {
    if (record.level < minLevel_) return;
    std::cerr << formatLogLine(record) << '\n';
}

void ConsoleSink::flush() { std::cerr.flush(); }

RotatingFileSink::RotatingFileSink(fs::path dir, std::string name, LogLevel minLevel, std::uintmax_t maxBytes,
                                   int maxFiles)
    : dir_(std::move(dir)),
      name_(std::move(name)),
      minLevel_(minLevel),
      maxBytes_(std::max<std::uintmax_t>(maxBytes, 1024)),
      maxFiles_(std::max(maxFiles, 0)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    open();
}

fs::path RotatingFileSink::pathFor(int index) const {
    return dir_ / (index == 0 ? name_ + ".log" : name_ + "." + std::to_string(index) + ".log");
}

void RotatingFileSink::open() {
    std::error_code ec;
    const fs::path path = pathFor(0);
    size_ = fs::exists(path, ec) ? fs::file_size(path, ec) : 0;
    if (ec) size_ = 0;
    file_.open(path, std::ios::binary | std::ios::app);  // 追加：重启后接着写，不覆盖以前的日志
}

void RotatingFileSink::rotate() {
    file_.close();
    std::error_code ec;
    if (maxFiles_ == 0) {
        fs::remove(pathFor(0), ec);
    } else {
        fs::remove(pathFor(maxFiles_), ec);  // 最旧的一个删掉
        for (int i = maxFiles_ - 1; i >= 0; --i) {
            if (fs::exists(pathFor(i), ec)) fs::rename(pathFor(i), pathFor(i + 1), ec);
        }
    }
    open();
}

void RotatingFileSink::write(const LogRecord& record) {
    if (!file_.is_open() || record.level < minLevel_) return;
    const std::string line = formatLogLine(record);
    if (size_ > 0 && size_ + line.size() + 1 > maxBytes_) {
        rotate();
        if (!file_.is_open()) return;
    }
    file_ << line << '\n';
    file_.flush();
    size_ += line.size() + 1;
}

void RotatingFileSink::flush() {
    if (file_.is_open()) file_.flush();
}

fs::path setupLogging(const LoggingOptions& options) {
    Logger& logger = Logger::global();
    logger.clearSinks();

    LogLevel lowest = LogLevel::Off;
    fs::path logFile;
    if (!options.dir.empty() && options.level != LogLevel::Off) {
        auto file = std::make_shared<RotatingFileSink>(options.dir, options.name, options.level);
        if (file->ok()) {
            logFile = file->currentPath();
            logger.addSink(std::move(file));
            lowest = std::min(lowest, options.level);
        }
    }
    if (options.consoleLevel != LogLevel::Off) {
        logger.addSink(std::make_shared<ConsoleSink>(options.consoleLevel));
        lowest = std::min(lowest, options.consoleLevel);
    }
    // 全局级别取两者中较低的，用来尽早丢弃谁都不要的日志；每个接收器再按自己的级别过滤。
    logger.setLevel(lowest);
    return logFile;
}

}  // namespace vision::logging
