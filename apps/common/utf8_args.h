#pragma once

// 两个程序（vision-cli、vision-runtime）共用：取得 UTF-8 编码的命令行参数。

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h 要在 shellapi.h 之前
#include <shellapi.h>
#endif

namespace vision {

// 取得 UTF-8 编码的命令行参数，并让控制台按 UTF-8 显示。
// Windows 上 main 的 argv 是本地编码（中文系统为 GBK），控制台默认也不是 UTF-8，
// 不处理的话，中文参数会变成乱码，中文输出也会显示成乱码。
inline std::vector<std::string> utf8Args(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::vector<std::string> args;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) return args;
    for (int i = 1; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(size > 0 ? static_cast<size_t>(size - 1) : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), size, nullptr, nullptr);
        args.push_back(std::move(arg));
    }
    LocalFree(wide);
    return args;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

}  // namespace vision
