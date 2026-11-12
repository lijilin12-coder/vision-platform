#include "vision/crash/crash_handler.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <exception>
#include <string>
#include <system_error>

#include "vision/common/log.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <crtdbg.h>
#include <csignal>
#include <dbghelp.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define VISION_HAS_BACKTRACE 1
#endif
#endif

namespace vision::crash {

namespace {

// 未捕获的异常：把异常信息写进日志，然后 abort（由下面的崩溃处理接手）。
[[noreturn]] void onTerminate() {
    std::string what = "未知类型的异常";
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        log::error("crash", "未捕获的异常，程序即将终止", {{"what", what}});
    } else {
        log::error("crash", "std::terminate 被调用，程序即将终止");
    }
    Logger::global().flush();
    std::abort();
}

std::string startStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec);
    return buf;
}

}  // namespace

#ifdef _WIN32

// ============================== Windows ==============================

namespace {

wchar_t g_dumpPath[MAX_PATH * 2] = L"";
wchar_t g_reportPath[MAX_PATH * 2] = L"";

void writeDump(EXCEPTION_POINTERS* exception) {
    const HANDLE file =
        CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception;
    info.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpWithThreadInfo);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, exception ? &info : nullptr, nullptr,
                      nullptr);
    CloseHandle(file);
}

void writeReport(const char* reason, DWORD code, const void* address) {
    const HANDLE file =
        CreateFileW(g_reportPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[256];
    const int n = std::snprintf(text, sizeof(text),
                                "vision crash report\nreason: %s\nexception code: 0x%08lX\naddress: %p\npid: %lu\n"
                                "Open the .dmp file next to this report with Visual Studio or WinDbg.\n",
                                reason, static_cast<unsigned long>(code), address,
                                static_cast<unsigned long>(GetCurrentProcessId()));
    DWORD written = 0;
    if (n > 0) WriteFile(file, text, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(file);
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* exception) {
    const DWORD code = exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionCode : 0;
    const void* address = exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionAddress : nullptr;
    writeReport("unhandled exception", code, address);
    writeDump(exception);
    return EXCEPTION_EXECUTE_HANDLER;  // 结束进程，退出码为异常代码
}

void onAbort(int) {
    writeReport("abort", 0, nullptr);
    writeDump(nullptr);
    _exit(3);
}

}  // namespace

CrashHandlerInfo installCrashHandler(const CrashHandlerOptions& options) {
    std::error_code ec;
    std::filesystem::create_directories(options.dir, ec);

    const std::string base =
        "crash-" + options.appName + "-" + startStamp() + "-" + std::to_string(GetCurrentProcessId());
    CrashHandlerInfo info;
    info.dumpPath = options.dir / (base + ".dmp");
    info.reportPath = options.dir / (base + ".txt");
    wcsncpy_s(g_dumpPath, info.dumpPath.wstring().c_str(), _TRUNCATE);
    wcsncpy_s(g_reportPath, info.reportPath.wstring().c_str(), _TRUNCATE);

    // 崩溃时不要弹出对话框：无人值守的产线上，对话框会让程序一直卡在那里，没法自动重启。
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif

    SetUnhandledExceptionFilter(onUnhandledException);
    std::signal(SIGABRT, onAbort);
    std::set_terminate(onTerminate);
    return info;
}

#else

// ============================== macOS / Linux ==============================

namespace {

char g_reportPath[2048] = "";
char g_altStack[64 * 1024];  // 备用栈：栈溢出时，原来的栈已经用不了了

// 信号处理函数里只能调用“异步信号安全”的函数：不能用 printf、不能申请内存、不能加锁。
// 所以这里只用 open / write / close，并且自己把数字转成文本。
void writeText(int fd, const char* text) {
    const ssize_t ignored = write(fd, text, std::strlen(text));
    (void)ignored;
}

void writeNumber(int fd, unsigned long long value, int base) {
    char buf[32];
    int pos = static_cast<int>(sizeof(buf));
    buf[--pos] = '\0';
    do {
        const int digit = static_cast<int>(value % static_cast<unsigned>(base));
        buf[--pos] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= static_cast<unsigned>(base);
    } while (value != 0 && pos > 0);
    writeText(fd, buf + pos);
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        case SIGBUS: return "SIGBUS";
        default: return "?";
    }
}

const char* signalMeaning(int sig) {
    switch (sig) {
        case SIGSEGV: return "invalid memory access";
        case SIGABRT: return "abort() called: failed assertion or uncaught exception";
        case SIGFPE: return "arithmetic error, e.g. integer division by zero";
        case SIGILL: return "illegal instruction";
        case SIGBUS: return "bus error: bad memory alignment or mapping";
        default: return "";
    }
}

void writeReportTo(int fd, int sig, const siginfo_t* info) {
    writeText(fd, "vision crash report\nsignal: ");
    writeNumber(fd, static_cast<unsigned long long>(sig), 10);
    writeText(fd, " (");
    writeText(fd, signalName(sig));
    writeText(fd, ") ");
    writeText(fd, signalMeaning(sig));
    writeText(fd, "\n");
    if (info != nullptr && (sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL)) {
        writeText(fd, "fault address: 0x");
        writeNumber(fd, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info->si_addr)), 16);
        writeText(fd, "\n");
    }
    writeText(fd, "pid: ");
    writeNumber(fd, static_cast<unsigned long long>(getpid()), 10);
    writeText(fd, "\nbacktrace:\n");
#ifdef VISION_HAS_BACKTRACE
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, fd);
#else
    writeText(fd, "  (not available on this platform; use the core file or the system fault log)\n");
#endif
}

void onSignal(int sig, siginfo_t* info, void*) {
    const int fd = open(g_reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        writeReportTo(fd, sig, info);
        close(fd);
    }
    writeText(STDERR_FILENO, "\n*** vision: crashed, report written to ");
    writeText(STDERR_FILENO, g_reportPath);
    writeText(STDERR_FILENO, "\n");

    // 安装时用了 SA_RESETHAND：进入这里时，这个信号的处理方式已经恢复成系统默认。
    // 重新发出同一个信号，让系统按默认方式结束进程并生成 core 文件。
    raise(sig);
}

}  // namespace

CrashHandlerInfo installCrashHandler(const CrashHandlerOptions& options) {
    std::error_code ec;
    std::filesystem::create_directories(options.dir, ec);

    CrashHandlerInfo info;
    info.reportPath =
        options.dir / ("crash-" + options.appName + "-" + startStamp() + "-" + std::to_string(getpid()) + ".txt");
    std::snprintf(g_reportPath, sizeof(g_reportPath), "%s", info.reportPath.u8string().c_str());

    if (options.enableCoreDumps) {
        rlimit limit{};
        if (getrlimit(RLIMIT_CORE, &limit) == 0) {
            limit.rlim_cur = limit.rlim_max;  // 把软限制提高到硬限制允许的最大值
            info.coreDumpsEnabled = setrlimit(RLIMIT_CORE, &limit) == 0 && limit.rlim_cur != 0;
        }
    } else {
        rlimit limit{0, 0};
        setrlimit(RLIMIT_CORE, &limit);
    }

    stack_t altStack{};
    altStack.ss_sp = g_altStack;
    altStack.ss_size = sizeof(g_altStack);
    sigaltstack(&altStack, nullptr);

    struct sigaction action {};
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) sigaction(sig, &action, nullptr);

    std::set_terminate(onTerminate);
    return info;
}

#endif

}  // namespace vision::crash
