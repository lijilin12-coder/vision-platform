#pragma once

// 上线包和工位的 JSON 文件仓储（ADR-20）。
// 适配器可以依赖领域层：它实现的是领域层定义的仓储接口。

#include <filesystem>

#include "vision/release/api.h"
#include "vision/data/domain/dataset.h"
#include "vision/data/domain/sample.h"
#include "vision/models/domain/model_version.h"
#include "vision/quality/domain/inspection_record.h"
#include "vision/release/domain/golden_set.h"
#include "vision/release/domain/release.h"
#include "vision/station/domain/station.h"
#include "vision/training/domain/training_job.h"

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

// <root>/imported-recipes/<方案版本 ID>.json
// 从方案包导入的方案版本：只读的文档（发布语言），不是可以编辑的方案。
class JsonImportedRecipes final : public release::ImportedRecipes {
public:
    explicit JsonImportedRecipes(std::filesystem::path root);
    Status save(const authoring::RecipeVersionDocument& recipe) override;
    std::optional<authoring::RecipeVersionDocument> find(const std::string& recipeVersionId) const override;
    std::vector<std::string> listIds() const override;

private:
    std::filesystem::path dir_;
};

// <root>/inspection-records/<工位 ID>/<日期>.jsonl
// 检测记录：每个工位每天一个文件，一行一条（JSON），只往后追加。
// 为什么不是一条记录一个文件：一个工位一天可能有几万次检测。
// 为什么不是数据库：现在的查询只有“某个工位最近的若干条”，按天分文件足够；也不用为它引入一个依赖。
class JsonlInspectionRecordRepository final : public quality::InspectionRecordRepository {
public:
    explicit JsonlInspectionRecordRepository(std::filesystem::path root);
    Status append(const quality::InspectionRecord& record) override;
    std::vector<quality::InspectionRecord> query(const std::string& stationId, const quality::RecordFilter& filter) override;
    std::optional<quality::InspectionRecord> find(const std::string& stationId, const std::string& recordId) override;
    std::vector<quality::InspectionRecord> all(const std::string& stationId) override;

private:
    // 这个工位的记录文件，从新到旧
    std::vector<std::filesystem::path> files(const std::string& stationId) const;
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

// <root>/model-versions/<模型名>@v<N>.json。只有模型版本的信息；模型文件在内容仓库里
class JsonModelVersionRepository final : public models::ModelVersionRepository {
public:
    explicit JsonModelVersionRepository(std::filesystem::path root);
    Status save(const models::ModelVersion& version) override;
    std::optional<models::ModelVersion> find(const std::string& versionId) override;
    std::vector<std::string> listIds() override;

private:
    std::filesystem::path dir_;
};

// <root>/training-jobs/<任务ID>.json
class JsonTrainingJobRepository final : public training::TrainingJobRepository {
public:
    explicit JsonTrainingJobRepository(std::filesystem::path root);
    Status save(const training::TrainingJob& job) override;
    std::optional<training::TrainingJob> find(const training::TrainingJobId& id) override;
    std::vector<training::TrainingJobId> listIds() override;

private:
    std::filesystem::path dir_;
};

}  // namespace vision::persistence
