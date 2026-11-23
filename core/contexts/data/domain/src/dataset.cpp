#include "vision/data/domain/dataset.h"

#include <algorithm>
#include <map>
#include <memory>

namespace vision::data {

int DatasetVersion::count(Label label) const {
    return static_cast<int>(std::count_if(entries_.begin(), entries_.end(), [label](const DatasetEntry& e) { return e.label == label; }));
}

Result<Dataset> Dataset::create(DatasetId id, std::string name) {
    if (id.empty()) return Result<Dataset>::failure("data.dataset_id", "数据集 ID 不能为空");
    if (name.empty()) return Result<Dataset>::failure("data.dataset_name", "数据集必须有名称");
    Dataset d;
    d.id_ = std::move(id);
    d.name_ = std::move(name);
    return d;
}

Dataset Dataset::restore(DatasetId id, std::string name, std::vector<SampleId> samples, int nextVersion) {
    Dataset d;
    d.id_ = std::move(id);
    d.name_ = std::move(name);
    d.samples_ = std::move(samples);
    d.nextVersion_ = nextVersion;
    return d;
}

Status Dataset::add(const SampleId& sample) {
    if (sample.empty()) return Status::failure("data.sample_id", "样本 ID 不能为空");
    if (std::find(samples_.begin(), samples_.end(), sample) != samples_.end()) return Status::success();  // 已经在了
    samples_.push_back(sample);
    return Status::success();
}

Status Dataset::remove(const SampleId& sample) {
    const auto it = std::find(samples_.begin(), samples_.end(), sample);
    if (it == samples_.end()) return Status::failure("data.sample_not_in_dataset", "数据集里没有这个样本");
    samples_.erase(it);
    return Status::success();
}

Result<DatasetVersion> Dataset::freeze(const std::vector<SampleState>& states) {
    using R = Result<DatasetVersion>;
    if (samples_.empty()) return R::failure("data.dataset_empty", "空的数据集不能冻结");

    std::map<SampleId, SampleState> byId;
    for (const auto& s : states) byId[s.id] = s;
    std::vector<DatasetEntry> entries;
    std::string benchmark, unlabeled;
    int benchmarkCount = 0, unlabeledCount = 0;
    for (const auto& id : samples_) {
        const auto it = byId.find(id);
        if (it == byId.end()) return R::failure("data.sample_missing", "找不到样本 " + id.substr(0, 12) + "…");
        if (it->second.use == SampleUse::Benchmark) {  // N1
            if (benchmarkCount++ < 5) benchmark += (benchmark.empty() ? "" : "、") + id.substr(0, 12) + "…";
        }
        if (it->second.label == Label::Unlabeled) {
            if (unlabeledCount++ < 5) unlabeled += (unlabeled.empty() ? "" : "、") + id.substr(0, 12) + "…";
        }
        entries.push_back({id, it->second.label});
    }
    if (benchmarkCount > 0) {
        return R::failure("data.n1", "数据集里有 " + std::to_string(benchmarkCount) +
                                         " 个样本已经留作基准（" + benchmark + (benchmarkCount > 5 ? " 等" : "") +
                                         "）。留作基准的样本不能用于训练：否则模型等于见过验收用的图像");
    }
    if (unlabeledCount > 0) {
        return R::failure("data.unlabeled", "数据集里有 " + std::to_string(unlabeledCount) + " 个样本还没有标签（" + unlabeled +
                                                (unlabeledCount > 5 ? " 等" : "") + "）");
    }

    const int number = nextVersion_++;
    DatasetVersion version(id_ + "@v" + std::to_string(number), id_, number, name_, std::move(entries));
    record(std::make_shared<DatasetVersionFrozen>(version.id(), static_cast<int>(version.entries().size())));
    return version;
}

}  // namespace vision::data
