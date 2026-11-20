#pragma once

// JSON 文件读写的公共部分，只在本适配器内部使用。

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>

#include "vision/common/path.h"
#include "vision/common/result.h"

namespace vision::persistence::detail {

inline Status writeFileAtomically(const std::filesystem::path& path, const std::string& content) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return Status::failure("persistence.write", "无法写入 " + pathToUtf8(tmp));
        out << content;
        if (!out) return Status::failure("persistence.write", "写入失败 " + pathToUtf8(tmp));
    }
    fs::rename(tmp, path, ec);  // 先写临时文件再改名，避免写到一半时程序崩溃留下损坏的文件
    if (ec) return Status::failure("persistence.write", "无法替换 " + pathToUtf8(path) + "：" + ec.message());
    return Status::success();
}

inline std::optional<nlohmann::json> readJson(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return nlohmann::json::parse(ss.str());
}

// ID 会被用作文件名：不允许为空，也不允许带路径分隔符
inline bool safeFileName(const std::string& id) {
    return !id.empty() && id.find('/') == std::string::npos && id.find('\\') == std::string::npos && id != "." && id != "..";
}

}  // namespace vision::persistence::detail
