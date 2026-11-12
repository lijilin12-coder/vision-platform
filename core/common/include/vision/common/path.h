#pragma once

// 约定：程序内部所有表示路径的 std::string 都是 UTF-8 编码。
//
// 为什么需要这个文件：在 Windows 上，直接用 std::string 构造 std::filesystem::path 时，
// 字符串会按系统的本地编码（中文系统是 GBK）解释，而不是 UTF-8。带中文的路径就会变成乱码，
// 文件读写失败。反过来，path.string() 在遇到本地编码表示不了的字符时还会抛异常。
// 所以路径和字符串之间的转换一律通过下面两个函数。macOS 和 Linux 上它们没有额外开销。

#include <filesystem>
#include <string>

namespace vision {

inline std::filesystem::path pathFromUtf8(const std::string& utf8) { return std::filesystem::u8path(utf8); }

inline std::string pathToUtf8(const std::filesystem::path& path) { return path.u8string(); }

}  // namespace vision
