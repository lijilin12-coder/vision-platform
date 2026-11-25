#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace vision {

// 毫秒时间（1970-01-01 UTC 以来）→ "2026-11-24"（UTC）。
// 公历日期的换算（Howard Hinnant 的算法）：不依赖系统的时区设置，各平台结果一样。
inline std::string utcDay(std::int64_t timeMs) {
    const std::int64_t days = timeMs / 86400000;
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d", year, month, day);
    return buffer;
}

}  // namespace vision
