#pragma once

// 上线包和工位的 JSON 文件仓储（ADR-20）。
// 适配器可以依赖领域层：它实现的是领域层定义的仓储接口。

#include <filesystem>

#include "vision/data/domain/dataset.h"
#include "vision/data/domain/sample.h"
#include "vision/release/domain/golden_set.h"
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

// <root>/golden-sets/<goldenSetId>.json
// 只保存图像的引用（路径），不复制图像文件。
class JsonGoldenSetRepository final : public release::GoldenSetRepository {
public:
    explicit JsonGoldenSetRepository(std::filesystem::path root);
    Status save(const release::GoldenSet& set) override;
    std::optional<release::GoldenSet> find(const release::GoldenSetId& id) override;
    std::vector<release::GoldenSetId> listIds() override;

private:
    std::filesystem::path dir_;
};

// <root>/samples/<样本ID>.json。只有样本的信息；图像内容在内容仓库里（ADR-20）
class JsonSampleRepository final : public data::SampleRepository {
public:
    explicit JsonSampleRepository(std::filesystem::path root);
    Status save(const data::Sample& sample) override;
    std::optional<data::Sample> find(const data::SampleId& id) override;
    std::vector<data::SampleId> listIds() override;

private:
    std::filesystem::path dir_;
};

// <root>/datasets/<数据集ID>.json，<root>/dataset-versions/<数据集ID>/v<N>.json
class JsonDatasetRepository final : public data::DatasetRepository {
public:
    explicit JsonDatasetRepository(std::filesystem::path root);
    Status save(const data::Dataset& dataset) override;
    std::optional<data::Dataset> find(const data::DatasetId& id) override;
    std::vector<data::DatasetId> listIds() override;
    Status saveVersion(const data::DatasetVersion& version) override;
    std::optional<data::DatasetVersion> findVersion(const std::string& versionId) override;

private:
    std::filesystem::path dir_, versionsDir_;
};

}  // namespace vision::persistence
