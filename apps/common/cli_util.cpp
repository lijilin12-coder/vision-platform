// 两个命令行程序（vision-cli、vision-line）共用的小工具。

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "line_commands.h"
#include "vision/common/log.h"
#include "vision/common/path.h"

namespace vision::cli {

namespace fs = std::filesystem;

void printResultItems(const ResultItems& result) {
    const auto number = [](double v) {
        std::ostringstream os;
        os << std::fixed << std::setprecision(1) << v;
        return os.str();
    };
    constexpr std::size_t kShown = 5;
    for (std::size_t i = 0; i < result.defects.size() && i < kShown; ++i) {
        const auto& d = result.defects[i];
        std::cout << "    缺陷 " << d.category << "：位置 (" << number(d.centerX) << ", " << number(d.centerY) << ")，大小 " << d.width
                  << "×" << d.height << "，面积 " << number(d.area) << "\n";
    }
    if (result.defects.size() > kShown) std::cout << "    ……还有 " << result.defects.size() - kShown << " 个缺陷\n";
    for (const auto& m : result.measurements) {
        std::cout << "    测量 " << m.name << " = " << number(m.value) << (m.unit.empty() ? "" : " " + m.unit) << (m.pass ? "" : "（超出范围）")
                  << "\n";
    }
    for (const auto& p : result.poses) {
        if (p.found) {
            std::cout << "    位姿 (" << number(p.pose.x) << ", " << number(p.pose.y) << ")，旋转 " << number(p.pose.angle) << "°，相似度 "
                      << number(p.score * 100) << "%\n";
        } else {
            std::cout << "    位姿 没有找到（最好的相似度 " << number(p.score * 100) << "%）\n";
        }
    }
}


int fail(const Error& e) {
    log::error("cli", "命令失败", {{"code", e.code}, {"message", e.message}});
    std::cerr << "错误：" << e.message << "  (" << e.code << ")\n";
    return 1;
}

std::vector<fs::path> expandImages(const std::vector<std::string>& args) {
    std::vector<fs::path> out;
    for (const auto& a : args) {
        const fs::path p = pathFromUtf8(a);
        if (fs::is_directory(p)) {
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(p)) {
                const auto ext = pathToUtf8(e.path().extension());
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp") files.push_back(e.path());
            }
            std::sort(files.begin(), files.end());
            out.insert(out.end(), files.begin(), files.end());
        } else {
            out.push_back(p);
        }
    }
    return out;
}

}  // namespace vision::cli
