#include "vision/training/api.h"

#include "vision/common/id.h"
#include "vision/common/log.h"
#include "vision/training/domain/training_job.h"

namespace vision::training {

namespace {

JobView toView(const TrainingJob& job, double progress) {
    const JobState& s = job.state();
    JobView v;
    v.id = s.id;
    v.datasetVersionId = s.datasetVersionId;
    v.modelKind = s.config.modelKind;
    v.modelName = s.config.modelName;
    v.split = s.split.text();
    v.backend = s.backend;
    v.status = toString(s.status);
    v.failure = s.failure;
    v.progress = s.status == JobStatus::Completed ? 1.0 : progress;
    if (s.report) {
        v.metrics = s.report->metrics;
        v.seconds = s.report->seconds;
    }
    v.modelVersionId = s.modelVersionId;
    return v;
}

Status notFound(const std::string& id) { return Status::failure("training.job_not_found", "找不到训练任务 " + id); }

}  // namespace

TrainingService::TrainingService(TrainingJobRepository& jobs, TrainingBackend& backend, DatasetSource& datasets,
                                 ModelImporter& models, EventPublisher& events)
    : jobs_(jobs), backend_(backend), datasets_(datasets), models_(models), events_(events) {}

Result<std::string> TrainingService::submit(const SubmitRequest& request) {
    auto dataset = datasets_.load(request.datasetVersionId);
    if (!dataset) return dataset.error();

    DatasetFacts facts;
    facts.datasetVersionId = dataset.value().datasetVersionId;
    facts.frozen = dataset.value().frozen;
    for (const auto& s : dataset.value().samples) {
        ++(s.ng ? facts.ngCount : facts.okCount);
        if (s.benchmark) ++facts.benchmarkSamples;
    }
    const SplitSpec split{request.train, request.validation, request.test, request.seed};
    auto created = TrainingJob::submit(newUuid(), facts, {request.modelKind, request.modelName, request.settings}, split, backend_.name());
    if (!created) return created.error();  // T-1、T-2、T-3、样本太少：不产生任务
    TrainingJob job = std::move(created).value();

    // 划分（T-3）：由领域规则决定，训练后端拿到的是已经分好的样本
    std::vector<std::pair<std::string, bool>> labelled;
    for (const auto& s : dataset.value().samples) labelled.emplace_back(s.sampleId, s.ng);
    const auto splits = assignSplits(labelled, split);
    TrainingRequest training;
    training.jobId = job.id();
    training.config = job.state().config;
    for (std::size_t i = 0; i < dataset.value().samples.size(); ++i) {
        const auto& s = dataset.value().samples[i];
        training.samples.push_back({s.sampleId, s.ng, splits[i], s.contentPath});
    }

    std::vector<EventPtr> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto st = jobs_.save(job); !st) return st.error();  // 先记下“已提交”，再去找训练后端
        // 训练后端连不上不是“提交被拒绝”：任务是合规的，只是没能开始。记为失败，写明原因。
        if (auto ticket = backend_.start(training)) {
            (void)job.markStarted(ticket.value());
        } else {
            (void)job.fail("训练后端不可用：" + ticket.error().message);
            log::warn("training", "训练任务没有能开始", {{"job", job.id()}, {"reason", ticket.error().message}});
        }
        if (auto st = jobs_.save(job); !st) return st.error();
        events = job.pullEvents();
    }
    events_.publishAll(events);
    return job.id();
}

Result<JobView> TrainingService::refresh(const std::string& jobId) {
    std::vector<EventPtr> events;
    std::optional<JobView> view;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto job = jobs_.find(jobId);
        if (!job) return notFound(jobId).error();
        if (job->status() != JobStatus::Running) return toView(*job, progress_[jobId]);  // 已经结束（T-4），或者没有开始

        auto polled = backend_.poll(job->state().backendTicket);
        if (!polled) {
            // 这一次没有问到（训练后端暂时连不上）：任务的状态不变，下次再问。训练可能还在进行
            log::warn("training", "查询训练进度失败", {{"job", jobId}, {"reason", polled.error().message}});
            return toView(*job, progress_[jobId]);
        }
        const BackendProgress& p = polled.value();
        progress_[jobId] = p.progress;
        if (p.state == BackendProgress::State::Failed) {
            (void)job->fail(p.failure.empty() ? "训练失败" : p.failure);
        } else if (p.state == BackendProgress::State::Completed) {
            if (auto st = job->complete(p.report, p.output); !st) {
                (void)job->fail("训练后端报告完成，但结果不完整：" + st.error().message);
            } else {
                // 规则：训练任务已完成 → 把模型导入模型仓库，成为一个未发布的模型版本，来源记为这次训练（T-5）
                const auto provenance = job->provenance();
                const auto& out = *job->state().output;
                auto modelVersion = models_.importTrained({job->state().config.modelName, out.modelFile, out.contractKind, out.inputWidth,
                                                           out.inputHeight, out.inputChannels, out.outputMeaning, out.numbers,
                                                           provenance.trainingJobId, provenance.datasetVersionId, provenance.split,
                                                           provenance.backend});
                if (modelVersion) {
                    (void)job->recordModelVersion(modelVersion.value());
                } else {
                    // 训练是成功的，只是没能导入：任务仍然是“已完成”，模型文件还在，原因写进日志，可以以后手工导入
                    log::error("training", "训练完成，但模型没有能导入模型仓库", {{"job", jobId}, {"reason", modelVersion.error().message}});
                }
            }
        }
        if (auto st = jobs_.save(*job); !st) return st.error();
        events = job->pullEvents();
        view = toView(*job, p.progress);
    }
    events_.publishAll(events);
    return *view;
}

Status TrainingService::cancel(const std::string& jobId) {
    std::vector<EventPtr> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto job = jobs_.find(jobId);
        if (!job) return notFound(jobId);
        const std::string ticket = job->state().backendTicket;
        const bool running = job->status() == JobStatus::Running;
        if (auto st = job->cancel(); !st) return st;  // T-4：已经结束的不能取消
        // 通知训练后端停下来。通知不到也算取消成功：我们这边不再等它的结果
        if (running) {
            if (auto st = backend_.cancel(ticket); !st) log::warn("training", "没有能通知训练后端停止", {{"job", jobId}, {"reason", st.error().message}});
        }
        if (auto st = jobs_.save(*job); !st) return st;
        events = job->pullEvents();
    }
    events_.publishAll(events);
    return Status::success();
}

Result<JobView> TrainingService::getJob(const std::string& jobId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto job = jobs_.find(jobId);
    if (!job) return notFound(jobId).error();
    const auto it = progress_.find(jobId);
    return toView(*job, it == progress_.end() ? 0.0 : it->second);
}

std::vector<std::string> TrainingService::listJobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.listIds();
}

}  // namespace vision::training
