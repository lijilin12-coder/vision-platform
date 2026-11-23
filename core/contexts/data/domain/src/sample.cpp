#include "vision/data/domain/sample.h"

#include <algorithm>
#include <memory>

namespace vision::data {

const char* toString(Label label) {
    switch (label) {
        case Label::Unlabeled: return "unlabeled";
        case Label::Ok: return "ok";
        case Label::Ng: return "ng";
    }
    return "?";
}

const char* toString(SampleUse use) {
    switch (use) {
        case SampleUse::Unassigned: return "unassigned";
        case SampleUse::Benchmark: return "benchmark";
        case SampleUse::Training: return "training";
    }
    return "?";
}

Result<Sample> Sample::import(SampleId contentHash, long long bytes, std::string originalName) {
    if (contentHash.empty()) return Result<Sample>::failure("data.sample_content", "样本没有内容");
    if (bytes <= 0) return Result<Sample>::failure("data.sample_content", "样本的内容是空的");
    Sample s;
    s.id_ = std::move(contentHash);
    s.bytes_ = bytes;
    if (!originalName.empty()) s.names_.push_back(std::move(originalName));
    s.record(std::make_shared<SampleImported>(s.id_));
    return s;
}

Sample Sample::restore(SampleId id, long long bytes, std::vector<std::string> names, Label label, SampleUse use) {
    Sample s;
    s.id_ = std::move(id);
    s.bytes_ = bytes;
    s.names_ = std::move(names);
    s.label_ = label;
    s.use_ = use;
    return s;
}

void Sample::seenAs(const std::string& name) {
    if (!name.empty() && std::find(names_.begin(), names_.end(), name) == names_.end()) names_.push_back(name);
}

Status Sample::setLabel(Label label) {
    if (label == label_) return Status::success();
    if (use_ != SampleUse::Unassigned) {
        return Status::failure("data.sample_in_use",
                               std::string("这个样本已经") + (use_ == SampleUse::Benchmark ? "留作基准" : "用于训练") + "，不能再改标签");
    }
    label_ = label;
    return Status::success();
}

Status Sample::reserveFor(SampleUse use) {
    if (use == SampleUse::Unassigned) return Status::failure("data.sample_use", "用途不能改回“未分配”");
    if (use_ == use) return Status::success();
    // N1
    if (use_ == SampleUse::Benchmark) {
        return Status::failure("data.n1", "样本 " + id_.substr(0, 12) + "… 已经留作基准，不能再用于训练");
    }
    if (use_ == SampleUse::Training) {
        return Status::failure("data.n1", "样本 " + id_.substr(0, 12) + "… 已经用于训练，不能再留作基准");
    }
    use_ = use;
    return Status::success();
}

}  // namespace vision::data
