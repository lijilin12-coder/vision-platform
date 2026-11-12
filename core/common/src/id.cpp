#include "vision/common/id.h"

#include <cstdint>
#include <cstdio>
#include <random>

namespace vision {

std::string newUuid() {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::uint64_t hi = dist(rng);
    std::uint64_t lo = dist(rng);
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;  // 版本 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;  // RFC 4122 变体

    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(hi >> 32), static_cast<unsigned>((hi >> 16) & 0xFFFF),
                  static_cast<unsigned>(hi & 0xFFFF), static_cast<unsigned>(lo >> 48),
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return buf;
}

}  // namespace vision
