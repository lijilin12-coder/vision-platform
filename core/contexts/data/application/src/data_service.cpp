#include "vision/data/api.h"

#include "vision/common/id.h"
#include "vision/data/domain/dataset.h"
#include "vision/data/domain/sample.h"

namespace vision::data {

namespace {

Result<Label> parseLabel(const std::string& text) {
    if (text == "ok" || text == "OK") return Label::Ok;
    if (text == "ng" || text == "NG") return Label::Ng;
    if (text.empty() || text == "unlabeled") return Label::Unlabeled;
    return Result<Label>::failure("data.bad_label", "标签只能是 ok 或 ng：" + text);
}

Status sampleNotFound(const std::string& id) { return Status::failure("data.sample_not_found", "找不到样本 " + id); }

std::string fileName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

DataService::DataService(SampleRepository& samples, DatasetRepository& datasets, SampleContent& content, EventPublisher& events)
    : samples_(samples), datasets_(datasets), content_(content), events_(events) {}

Result<std::string> DataService::importSample(const std::string& imagePath, const std::string& labelText) {
    using R = Result<std::string>;
    auto label = parseLabel(labelText);
    if (!label) return label.error();
    auto stored = content_.importFile(imagePath);
    if (!stored) return stored.error();

    std::vector<EventPtr> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 样本的 ID 就是内容的哈希：内容相同就是同一个样本，不管文件叫什么名字
        auto existing = samples_.find(stored.value().sha256);
        if (existing) {
            existing->seenAs(fileName(imagePath));
            if (label.value() != Label::Unlabeled) {
                if (existing->label() != Label::Unlabeled && existing->label() != label.value()) {
                    return R::failure("data.label_conflict", "这幅图像已经导入过，标签是 " + std::string(toString(existing->label())) +
                                                                 "，这次给的是 " + toString(label.value()) + "。请确认哪个是对的");
                }
                if (auto st = existing->setLabel(label.value()); !st) return st.error();
            }
            if (auto st = samples_.save(*existing); !st) return st.error();
            return existing->id();
        }
        auto sample = Sample::import(stored.value().sha256, stored.value().bytes, fileName(imagePath));
        if (!sample) return sample.error();
        if (auto st = sample.value().setLabel(label.value()); !st) return st.error();
        if (auto st = samples_.save(sample.value()); !st) return st.error();
        events = sample.value().pullEvents();
    }
    events_.publishAll(events);
    return stored.value().sha256;
}

Status DataService::labelSample(const std::string& sampleId, const std::string& labelText) {
    auto label = parseLabel(labelText);
    if (!label) return label.error();
    std::lock_guard<std::mutex> lock(mutex_);
    auto sample = samples_.find(sampleId);
    if (!sample) return sampleNotFound(sampleId);
    if (auto st = sample->setLabel(label.value()); !st) return st;
    return samples_.save(*sample);
}

Result<SampleView> DataService::getSample(const std::string& sampleId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto sample = samples_.find(sampleId);
    if (!sample) return sampleNotFound(sampleId).error();
    return SampleView{sample->id(), sample->names(), toString(sample->label()), toString(sample->use()), content_.pathOf(sample->id())};
}

std::vector<std::string> DataService::listSamples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.listIds();
}

Result<std::string> DataService::createDataset(const std::string& name) {
    auto dataset = Dataset::create(newUuid(), name);
    if (!dataset) return dataset.error();
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto st = datasets_.save(dataset.value()); !st) return st.error();
    return dataset.value().id();
}

Status DataService::addToDataset(const std::string& datasetId, const std::vector<std::string>& sampleIds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto dataset = datasets_.find(datasetId);
    if (!dataset) return Status::failure("data.dataset_not_found", "找不到数据集 " + datasetId);
    for (const auto& id : sampleIds) {
        if (!samples_.find(id)) return sampleNotFound(id);
        if (auto st = dataset->add(id); !st) return st;
    }
    return datasets_.save(*dataset);
}

Result<std::string> DataService::freezeDataset(const std::string& datasetId) {
    using R = Result<std::string>;
    std::vector<EventPtr> events;
    std::string versionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto dataset = datasets_.find(datasetId);
        if (!dataset) return R::failure("data.dataset_not_found", "找不到数据集 " + datasetId);

        // 把每个样本现在的标签和用途交给数据集聚合，由它检查（N1、都有标签）
        std::vector<Sample> samples;
        std::vector<Dataset::SampleState> states;
        for (const auto& id : dataset->samples()) {
            auto sample = samples_.find(id);
            if (!sample) return sampleNotFound(id).error();
            states.push_back({id, sample->label(), sample->use()});
            samples.push_back(std::move(*sample));
        }
        auto version = dataset->freeze(states);
        if (!version) return version.error();

        // 三个聚合（数据集版本、数据集、各个样本）分别保存。先保存样本的用途：
        // 万一中途失败，宁可样本被标成“用于训练”而没有对应的版本，也不要有一个版本里的样本还能被拿去做基准。
        for (auto& sample : samples) {
            if (auto st = sample.reserveFor(SampleUse::Training); !st) return st.error();
            if (auto st = samples_.save(sample); !st) return st.error();
        }
        if (auto st = datasets_.saveVersion(version.value()); !st) return st.error();
        if (auto st = datasets_.save(*dataset); !st) return st.error();
        events = dataset->pullEvents();
        versionId = version.value().id();
    }
    events_.publishAll(events);
    return versionId;
}

Result<DatasetVersionView> DataService::getDatasetVersion(const std::string& datasetVersionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto version = datasets_.findVersion(datasetVersionId);
    if (!version) return Result<DatasetVersionView>::failure("data.version_not_found", "找不到数据集版本 " + datasetVersionId);
    DatasetVersionView v;
    v.id = version->id();
    v.name = version->name();
    for (const auto& e : version->entries()) v.entries.push_back({e.sampleId, toString(e.label), content_.pathOf(e.sampleId)});
    v.okCount = version->count(Label::Ok);
    v.ngCount = version->count(Label::Ng);
    return v;
}

Status DataService::reserveForBenchmark(const std::vector<std::string>& sampleIds) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 先全部检查，再全部修改：有一个不行就一个都不改
    std::vector<Sample> samples;
    std::string training;
    int count = 0;
    for (const auto& id : sampleIds) {
        auto sample = samples_.find(id);
        if (!sample) return sampleNotFound(id);
        if (sample->use() == SampleUse::Training) {
            if (count++ < 5) training += (training.empty() ? "" : "、") + (sample->names().empty() ? id.substr(0, 12) + "…" : sample->names().front());
        }
        samples.push_back(std::move(*sample));
    }
    if (count > 0) {
        return Status::failure("data.n1", "有 " + std::to_string(count) + " 幅图像已经用于训练（" + training + (count > 5 ? " 等" : "") +
                                              "）。用于训练的图像不能留作基准：模型已经见过它们，验收结果会虚高");
    }
    for (auto& sample : samples) {
        if (auto st = sample.reserveFor(SampleUse::Benchmark); !st) return st;
        if (auto st = samples_.save(sample); !st) return st;
    }
    return Status::success();
}

std::vector<std::string> DataService::benchmarkSamplesAmong(const std::vector<std::string>& sampleIds) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& id : sampleIds) {
        const auto sample = samples_.find(id);
        if (sample && sample->use() == SampleUse::Benchmark) out.push_back(id);
    }
    return out;
}

}  // namespace vision::data
