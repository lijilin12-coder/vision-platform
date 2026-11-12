// 日志测试：格式、级别过滤、文件轮转、多线程写入。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "vision/common/log.h"
#include "vision/common/path.h"
#include "vision/logging/sinks.h"

using namespace vision;
namespace fs = std::filesystem;

namespace {

// 把收到的记录存在内存里，方便检查。
class MemorySink final : public LogSink {
public:
    void write(const LogRecord& record) override { records.push_back(record); }
    std::vector<LogRecord> records;
};

std::vector<std::string> readLines(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}

fs::path tempDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / pathFromUtf8("vision-日志测试-" + name);
    fs::remove_all(dir);
    return dir;
}

LogRecord recordAt(long long epochMillis, LogLevel level, std::string component, std::string message, LogFields fields = {}) {
    LogRecord r;
    r.time = std::chrono::system_clock::time_point(std::chrono::milliseconds(epochMillis));
    r.level = level;
    r.component = std::move(component);
    r.message = std::move(message);
    r.fields = std::move(fields);
    return r;
}

}  // namespace

TEST(LogFormat, OneLineWithTimestampLevelComponentAndFields) {
    // 2026-11-12T10:50:03.880Z
    const auto line = formatLogLine(recordAt(1794480603880LL, LogLevel::Info, "station", "热切换完成",
                                             {{"from", "R-1"}, {"to", "R-2"}}));
    EXPECT_EQ(line, "2026-11-12T10:50:03.880Z INFO  [station] 热切换完成 from=R-1 to=R-2");
}

TEST(LogFormat, QuotesAndEscapesSoARecordIsAlwaysOneLine) {
    const auto line = formatLogLine(recordAt(0, LogLevel::Error, "cli", "第一行\n第二行",
                                             {{"path", "C:\\数据 目录\\a.png"}, {"empty", ""}, {"quote", "说\"好\""}}));
    EXPECT_EQ(line.find('\n'), std::string::npos) << "换行必须被转义";
    EXPECT_NE(line.find("第一行\\n第二行"), std::string::npos);
    EXPECT_NE(line.find("path=\"C:\\\\数据 目录\\\\a.png\""), std::string::npos);
    EXPECT_NE(line.find("empty=\"\""), std::string::npos);
    EXPECT_NE(line.find("quote=\"说\\\"好\\\"\""), std::string::npos);
    EXPECT_EQ(line.rfind("1970-01-01T00:00:00.000Z ERROR [cli] ", 0), 0u);
}

TEST(LogLevelParsing, AcceptsNamesCaseInsensitively) {
    EXPECT_EQ(parseLogLevel("debug", LogLevel::Info), LogLevel::Debug);
    EXPECT_EQ(parseLogLevel("WARN", LogLevel::Info), LogLevel::Warn);
    EXPECT_EQ(parseLogLevel("warning", LogLevel::Info), LogLevel::Warn);
    EXPECT_EQ(parseLogLevel("off", LogLevel::Info), LogLevel::Off);
    EXPECT_EQ(parseLogLevel("不认识", LogLevel::Info), LogLevel::Info);
}

TEST(Logger, FiltersByLevel) {
    Logger logger;
    auto sink = std::make_shared<MemorySink>();
    logger.addSink(sink);
    logger.setLevel(LogLevel::Warn);

    logger.write(LogLevel::Debug, "t", "看不到");
    logger.write(LogLevel::Info, "t", "看不到");
    logger.write(LogLevel::Warn, "t", "看得到");
    logger.write(LogLevel::Error, "t", "看得到", {{"k", "v"}});

    ASSERT_EQ(sink->records.size(), 2u);
    EXPECT_EQ(sink->records[0].level, LogLevel::Warn);
    EXPECT_EQ(sink->records[1].fields.at(0).second, "v");
    EXPECT_TRUE(logger.enabled(LogLevel::Error));
    EXPECT_FALSE(logger.enabled(LogLevel::Info));

    logger.setLevel(LogLevel::Off);
    logger.write(LogLevel::Error, "t", "关闭后什么都不写");
    EXPECT_EQ(sink->records.size(), 2u);
}

TEST(Logger, CopiesTextSoTemporariesAreSafe) {
    // 写日志时传入的只是“视图”，不复制字符；确实要输出时，Logger 才把内容复制进记录。
    // 所以调用返回之后，原来的字符串即使被销毁或修改，记录里的内容也不受影响。
    Logger logger;
    auto sink = std::make_shared<MemorySink>();
    logger.addSink(sink);
    {
        std::string component = "station";
        std::string value = "R-1";
        logger.write(LogLevel::Info, component, std::string("热切换") + "完成", {{"from", value}, {"n", std::to_string(42)}});
        component = "xxxxxxx";
        value = "yyy";
    }
    ASSERT_EQ(sink->records.size(), 1u);
    EXPECT_EQ(sink->records[0].component, "station");
    EXPECT_EQ(sink->records[0].message, "热切换完成");
    EXPECT_EQ(sink->records[0].fields.at(0).second, "R-1");
    EXPECT_EQ(sink->records[0].fields.at(1).second, "42");
}

TEST(RotatingFileSink, AppendsAcrossRestarts) {
    const fs::path dir = tempDir("append");
    {
        logging::RotatingFileSink sink(dir, "app");
        ASSERT_TRUE(sink.ok());
        sink.write(recordAt(0, LogLevel::Info, "t", "第一次运行"));
    }
    {
        logging::RotatingFileSink sink(dir, "app");  // 模拟程序重启
        sink.write(recordAt(1000, LogLevel::Info, "t", "第二次运行"));
    }
    const auto lines = readLines(dir / "app.log");
    ASSERT_EQ(lines.size(), 2u) << "重启后应该接着写，不覆盖以前的日志";
    EXPECT_NE(lines[0].find("第一次运行"), std::string::npos);
    EXPECT_NE(lines[1].find("第二次运行"), std::string::npos);
    fs::remove_all(dir);
}

TEST(RotatingFileSink, RotatesBySizeAndKeepsLimitedFiles) {
    const fs::path dir = tempDir("rotate");
    {
        // 每个文件最多约 1 KB，最多保留 2 个旧文件
        logging::RotatingFileSink sink(dir, "app", LogLevel::Trace, 1024, 2);
        for (int i = 0; i < 200; ++i) {
            sink.write(recordAt(i, LogLevel::Info, "t", "一条大约五十个字节的日志", {{"i", std::to_string(i)}}));
        }
    }
    EXPECT_TRUE(fs::exists(dir / "app.log"));
    EXPECT_TRUE(fs::exists(dir / "app.1.log"));
    EXPECT_TRUE(fs::exists(dir / "app.2.log"));
    EXPECT_FALSE(fs::exists(dir / "app.3.log")) << "超过保留数量的旧文件应该被删除";

    std::uintmax_t total = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        EXPECT_LE(fs::file_size(entry.path()), 1024u) << "单个文件不超过上限";
        total += fs::file_size(entry.path());
    }
    EXPECT_LE(total, 3u * 1024u) << "日志占用的磁盘空间有上限";

    // 最新的日志在 app.log 里，并且最后一条是最后写入的那条
    const auto lines = readLines(dir / "app.log");
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines.back().find("i=199"), std::string::npos);
    fs::remove_all(dir);
}

TEST(RotatingFileSink, RespectsMinLevel) {
    const fs::path dir = tempDir("level");
    {
        logging::RotatingFileSink sink(dir, "app", LogLevel::Warn);
        sink.write(recordAt(0, LogLevel::Info, "t", "低于级别"));
        sink.write(recordAt(0, LogLevel::Error, "t", "达到级别"));
    }
    EXPECT_EQ(readLines(dir / "app.log").size(), 1u);
    fs::remove_all(dir);
}

TEST(Logger, ConcurrentWritesProduceWholeLines) {
    const fs::path dir = tempDir("threads");
    Logger logger;
    logger.setLevel(LogLevel::Trace);
    logger.addSink(std::make_shared<logging::RotatingFileSink>(dir, "app", LogLevel::Trace, 64 * 1024 * 1024, 1));

    const int threads = 4;
    const int perThread = 500;
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        // 线程编号和次数作为参数传给线程，而不是由 lambda 捕获。
        // 原因：perThread 是常量，“要不要捕获它”三个编译器的要求不一致：
        // MSVC 要求捕获，Clang 对多余的捕获给出警告。作为参数传递，三个编译器都没有意见。
        workers.emplace_back(
            [&logger](int thread, int count) {
                for (int i = 0; i < count; ++i) {
                    logger.write(LogLevel::Info, "worker", "并发写入",
                                 {{"thread", std::to_string(thread)}, {"i", std::to_string(i)}});
                }
            },
            t, perThread);
    }
    for (auto& w : workers) w.join();
    logger.clearSinks();  // 关闭文件

    const auto lines = readLines(dir / "app.log");
    ASSERT_EQ(lines.size(), static_cast<std::size_t>(threads * perThread)) << "不能丢日志";
    std::vector<int> next(threads, 0);
    for (const auto& line : lines) {
        // 每一行都必须是完整的一条：没有两条日志互相穿插
        ASSERT_NE(line.find(" INFO  [worker] 并发写入 thread="), std::string::npos) << line;
        const auto tPos = line.find("thread=") + 7;
        const auto iPos = line.find(" i=") + 3;
        const int t = std::stoi(line.substr(tPos));
        const int i = std::stoi(line.substr(iPos));
        ASSERT_EQ(i, next[static_cast<std::size_t>(t)]) << "同一个线程写的日志保持先后顺序";
        ++next[static_cast<std::size_t>(t)];
    }
    fs::remove_all(dir);
}
