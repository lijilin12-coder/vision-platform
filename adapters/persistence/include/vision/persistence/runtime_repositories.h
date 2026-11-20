#pragma once

// 上线包和工位的 JSON 文件仓储（ADR-20）。
// 适配器可以依赖领域层：它实现的是领域层定义的仓储接口。

#include <filesystem>

#include "vision/release/domain/release.h"
#include "vision/station/domain/station.h"

namespace vision::persistence {

// <root>/releases/<releaseId>.json
class JsonReleaseRepository final : public release::ReleaseRepository {
public:
    explicit JsonReleaseRepository(std::filesystem::path root);
    Status save(const release::Release& release) override;
    std::optional<release::Release> find(const release::ReleaseId& id) override;
    std::vector<release::ReleaseId> listIds() override;

private:
    std::filesystem::path dir_;
};

// <root>/stations/<stationId>.json
class JsonStationRepository final : public station::StationRepository {
public:
    explicit JsonStationRepository(std::filesystem::path root);
    Status save(const station::Station& station) override;
    std::optional<station::Station> find(const station::StationId& id) override;
    std::vector<station::StationId> listIds() override;

private:
    std::filesystem::path dir_;
};

}  // namespace vision::persistence
