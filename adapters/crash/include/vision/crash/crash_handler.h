#pragma once

// 崩溃处理：程序崩溃时留下现场，供事后分析。
//
//   Windows        写一个 minidump 文件（.dmp），可以用 Visual Studio 或 WinDbg 打开，
//                  看到崩溃时每个线程的调用栈和局部变量。需要配套的 .pdb 文件。
//   macOS / Linux  写一份文本崩溃报告（信号、出错地址、调用栈），
//                  并把 core 文件的大小限制放开，让系统生成 core 文件（是否生成、存到哪里由系统设置决定）。
//
// 两种平台都会额外处理“未捕获的异常”：先把异常信息写进日志，再按崩溃处理。

#include <filesystem>
#include <string>

namespace vision::crash {

struct CrashHandlerOptions {
    std::filesystem::path dir;        // 崩溃文件保存目录
    std::string appName = "vision";   // 用于文件名
    bool enableCoreDumps = true;      // macOS / Linux：放开 core 文件大小限制
};

struct CrashHandlerInfo {
    std::filesystem::path reportPath;  // 文本崩溃报告（崩溃时才会创建）
    std::filesystem::path dumpPath;    // Windows 的 minidump（崩溃时才会创建）；其他平台为空
    bool coreDumpsEnabled = false;     // macOS / Linux：是否成功放开了 core 文件大小限制
};

// 安装崩溃处理。在程序启动时调用一次。
CrashHandlerInfo installCrashHandler(const CrashHandlerOptions& options);

}  // namespace vision::crash
