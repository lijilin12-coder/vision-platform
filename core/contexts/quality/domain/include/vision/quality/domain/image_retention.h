#pragma once

#include <cstdint>
#include <string>

#include "vision/common/result.h"

namespace vision::quality {

// 一幅检测图像留多久
enum class ImageKeeping {
    Discard,    // 不留
    ShortTerm,  // 进短期缓存：过了保留期就清掉
    LongTerm,   // 长期保留
};

// 检测图像的保留规则（F-3，docs/ddd/18-image-cache.md）。
//
// 为什么要有“短期”这一档：图像比记录大得多，不能都永远留着；但漏检恰恰发生在判了 OK 的那些上——
// 判 OK 的图像一幅都不留的话，事后发现漏检时已经无图可看。所以判 OK 的先留一段时间，够人回头来复判。
//
//   M1 判 NG、ERROR 的图像长期保留：出了问题要能回头看
//   M2 判 OK 的图像进短期缓存，过了保留期清掉
//   M3 复判过的检测，图像转成长期保留（不管复判的结论是什么）：人看过、下过结论的，是以后改方案的材料
//   M4 清理只删图像，不动检测记录（Q1）
struct ImageRetention {
    bool keepRejected = true;   // 留不留判 NG、ERROR 的
    bool cacheAccepted = true;  // 判 OK 的进不进短期缓存
    std::int64_t retentionMs = 72LL * 3600 * 1000;  // 短期缓存留多久

    static Result<ImageRetention> create(bool keepRejected, bool cacheAccepted, double retentionHours);

    ImageKeeping at(const std::string& verdict) const {  // M1、M2
        if (verdict == "OK") return cacheAccepted ? ImageKeeping::ShortTerm : ImageKeeping::Discard;
        return keepRejected ? ImageKeeping::LongTerm : ImageKeeping::Discard;
    }
    // 短期缓存里，这个时刻之前的图像已经过期
    std::int64_t cutoff(std::int64_t nowMs) const { return nowMs - retentionMs; }
};

}  // namespace vision::quality
