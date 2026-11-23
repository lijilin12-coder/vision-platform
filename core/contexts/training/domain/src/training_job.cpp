#include "vision/training/domain/training_job.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>

namespace vision::training {

const char* toString(Split split) {
    switch (split) {
        case Split::Train: return "train";
        case Split::Validation: return "val";
        case Split::Test: return "test";
    }
    return "?";
}

const char* toString(JobStatus status) {
    switch (status) {
        case JobStatus::Submitted: return "submitted";
        case JobStatus::Running: return "running";
        case JobStatus::Completed: return "completed";
        case JobStatus::Failed: return "failed";
        case JobStatus::Cancelled: return "cancelled";
    }
    return "?";
}

Status SplitSpec::validate() const {
    if (train <= 0 || validation < 0 || test < 0) return Status::failure("training.bad_split", "训练集的比例必须大于 0，验证集和测试集不能是负数");
    if (std::abs(train + validation + test - 1.0) > 1e-6) return Status::failure("training.bad_split", "三部分的比例加起来必须是 1");
    return Status::success();
}

std::string SplitSpec::text() const {
    std::ostringstream os;
    os << train << "/" << validation << "/" << test << " seed=" << seed;
    return os.str();
}

std::vector<Split> assignSplits(const std::vector<std::pair<std::string, bool>>& samples, const SplitSpec& spec) {
    std::vector<Split> result(samples.size(), Split::Train);
    // 自己写的小伪随机数发生器：标准库的洗牌在不同平台上结果不一样，而划分必须可重现（T-3）
    std::uint64_t state = 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(spec.seed) + 1);
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (const bool ng : {false, true}) {
        std::vector<std::size_t> group;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            if (samples[i].second == ng) group.push_back(i);
        }
        // 先按样本 ID 排序再洗牌：结果和输入的顺序无关
        std::sort(group.begin(), group.end(), [&](std::size_t a, std::size_t b) { return samples[a].first < samples[b].first; });
        for (std::size_t i = group.size(); i > 1; --i) std::swap(group[i - 1], group[next() % i]);
        const auto n = static_cast<double>(group.size());
        const auto testCount = static_cast<std::size_t>(std::floor(n * spec.test + 0.5));
        const auto valCount = std::min(group.size() - testCount, static_cast<std::size_t>(std::floor(n * spec.validation + 0.5)));
        for (std::size_t i = 0; i < group.size(); ++i) {
            result[group[i]] = i < testCount ? Split::Test : i < testCount + valCount ? Split::Validation : Split::Train;
        }
    }
    return result;
}

Result<TrainingJob> TrainingJob::submit(TrainingJobId id, const DatasetFacts& dataset, TrainingConfig config, SplitSpec split,
                                        std::string backend) {
    using R = Result<TrainingJob>;
    if (id.empty()) return R::failure("training.empty_id", "训练任务 ID 不能为空");
    if (dataset.datasetVersionId.empty()) return R::failure("training.no_dataset", "没有指定数据集版本");
    // T-1
    if (!dataset.frozen) return R::failure("training.dataset_not_frozen", "只能用已经冻结的数据集版本训练：这样才说得清模型是用哪些数据训练的");
    // T-2（规则 N1）
    if (dataset.benchmarkSamples > 0) {
        return R::failure("training.n1", "数据集版本里有 " + std::to_string(dataset.benchmarkSamples) +
                                             " 个样本是留作基准的。留作基准的样本不能用于训练");
    }
    if (config.modelKind.empty()) return R::failure("training.bad_config", "没有指定训练哪一种模型");
    if (config.modelName.empty()) return R::failure("training.bad_config", "没有指定模型的名称");
    // T-3
    if (auto st = split.validate(); !st) return st.error();
    if (dataset.okCount < kMinOkSamples) {
        return R::failure("training.too_few_samples", "合格的样本只有 " + std::to_string(dataset.okCount) + " 个，至少需要 " +
                                                          std::to_string(kMinOkSamples) + " 个：模型要靠它们学习“正常是什么样”");
    }
    JobState s;
    s.id = std::move(id);
    s.datasetVersionId = dataset.datasetVersionId;
    s.config = std::move(config);
    s.split = split;
    s.backend = std::move(backend);
    TrainingJob job(std::move(s));
    job.record(std::make_shared<TrainingJobSubmitted>(job.s_.id, job.s_.datasetVersionId));
    return job;
}

TrainingJob TrainingJob::restore(JobState state) { return TrainingJob(std::move(state)); }

// T-4
Status TrainingJob::requireNotFinished() const {
    if (finished()) return Status::failure("training.finished", std::string("训练任务已经结束（") + toString(s_.status) + "），不能再修改");
    return Status::success();
}

Status TrainingJob::markStarted(std::string backendTicket) {
    if (auto st = requireNotFinished(); !st) return st;
    if (s_.status != JobStatus::Submitted) return Status::failure("training.already_started", "训练任务已经开始了");
    s_.status = JobStatus::Running;
    s_.backendTicket = std::move(backendTicket);
    record(std::make_shared<TrainingJobStarted>(s_.id));
    return Status::success();
}

Status TrainingJob::complete(TrainingReport report, TrainingOutput output) {
    if (auto st = requireNotFinished(); !st) return st;
    if (s_.status != JobStatus::Running) return Status::failure("training.not_running", "训练任务还没有开始");
    if (output.modelFile.empty()) return Status::failure("training.no_output", "训练完成了，却没有模型文件");
    if (output.contractKind.empty() || output.inputWidth <= 0 || output.inputHeight <= 0) {
        return Status::failure("training.no_contract", "训练完成了，却没有说明模型的输入输出规格");
    }
    s_.status = JobStatus::Completed;
    s_.report = std::move(report);
    s_.output = std::move(output);
    record(std::make_shared<TrainingJobCompleted>(s_.id, s_.output->modelFile));
    return Status::success();
}

Status TrainingJob::fail(const std::string& reason) {
    if (auto st = requireNotFinished(); !st) return st;
    s_.status = JobStatus::Failed;
    s_.failure = reason;
    record(std::make_shared<TrainingJobFailed>(s_.id, reason));
    return Status::success();
}

Status TrainingJob::cancel() {
    if (auto st = requireNotFinished(); !st) return st;
    s_.status = JobStatus::Cancelled;
    record(std::make_shared<TrainingJobCancelled>(s_.id));
    return Status::success();
}

Status TrainingJob::recordModelVersion(const std::string& modelVersionId) {
    if (s_.status != JobStatus::Completed) return Status::failure("training.not_completed", "只有完成的训练任务才有模型版本");
    if (!s_.modelVersionId.empty()) return Status::failure("training.model_recorded", "这次训练的模型版本已经记录过了");
    if (modelVersionId.empty()) return Status::failure("training.model_recorded", "模型版本 ID 不能为空");
    s_.modelVersionId = modelVersionId;
    return Status::success();
}

}  // namespace vision::training
