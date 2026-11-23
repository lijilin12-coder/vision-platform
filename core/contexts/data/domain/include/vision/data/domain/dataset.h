#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"
#include "vision/data/domain/sample.h"

namespace vision::data {

using DatasetId = std::string;

// 数据集版本里的一条：样本，以及冻结时它的标签
struct DatasetEntry {
    SampleId sampleId;
    Label label = Label::Unlabeled;
};

// 聚合根：数据集版本。冻结之后不可修改的一组样本，交给训练用（只有只读访问）。
// ID 形如 "<数据集ID>@v1"。
class DatasetVersion {
public:
    DatasetVersion(std::string id, DatasetId dataset, int number, std::string name, std::vector<DatasetEntry> entries)
        : id_(std::move(id)), dataset_(std::move(dataset)), number_(number), name_(std::move(name)), entries_(std::move(entries)) {}

    const std::string& id() const { return id_; }
    const DatasetId& dataset() const { return dataset_; }
    int number() const { return number_; }
    const std::string& name() const { return name_; }
    const std::vector<DatasetEntry>& entries() const { return entries_; }
    int count(Label label) const;

private:
    std::string id_;
    DatasetId dataset_;
    int number_;
    std::string name_;
    std::vector<DatasetEntry> entries_;
};

// 聚合根：数据集。可以反复修改的一组样本；冻结一次，得到一个数据集版本。
//
// 不变量：
//   - 一个样本在数据集里只出现一次
//   - 空的数据集不能冻结
// 规则 N1（含有留作基准的样本则拒绝冻结）涉及样本聚合，由应用层在冻结之前检查：
// freeze() 需要调用者把每个样本当前的标签和用途交进来，由它来拒绝。
class Dataset : public EventRecorder {
public:
    static Result<Dataset> create(DatasetId id, std::string name);
    static Dataset restore(DatasetId id, std::string name, std::vector<SampleId> samples, int nextVersion);

    Status add(const SampleId& sample);
    Status remove(const SampleId& sample);

    // 冻结时每个样本的状态
    struct SampleState {
        SampleId id;
        Label label = Label::Unlabeled;
        SampleUse use = SampleUse::Unassigned;
    };
    // 冻结：生成一个数据集版本。states 必须正好对应数据集里的全部样本。
    //   N1：有样本已经留作基准 → 拒绝，并列出是哪些
    //   有样本还没有标签 → 拒绝（训练需要知道每一张是合格还是不合格）
    Result<DatasetVersion> freeze(const std::vector<SampleState>& states);

    const DatasetId& id() const { return id_; }
    const std::string& name() const { return name_; }
    const std::vector<SampleId>& samples() const { return samples_; }
    int nextVersion() const { return nextVersion_; }

private:
    Dataset() = default;
    DatasetId id_;
    std::string name_;
    std::vector<SampleId> samples_;
    int nextVersion_ = 1;
};

struct DatasetVersionFrozen final : DomainEvent {
    std::string datasetVersionId;
    int sampleCount;
    DatasetVersionFrozen(std::string id, int n) : datasetVersionId(std::move(id)), sampleCount(n) {}
    const char* name() const override { return "DatasetVersionFrozen"; }
};

class DatasetRepository {
public:
    virtual ~DatasetRepository() = default;
    virtual Status save(const Dataset& dataset) = 0;
    virtual std::optional<Dataset> find(const DatasetId& id) = 0;
    virtual std::vector<DatasetId> listIds() = 0;
    virtual Status saveVersion(const DatasetVersion& version) = 0;
    virtual std::optional<DatasetVersion> findVersion(const std::string& versionId) = 0;
};

}  // namespace vision::data
