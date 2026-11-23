#pragma once

// BC-9 数据的公开接口（训练闭环需要的最小部分）。

#include <mutex>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::data {

class SampleRepository;   // 领域层
class DatasetRepository;  // 领域层

// ===== 端口：保存样本的图像内容 =====
// 把一个图像文件原样存进内容仓库，返回内容的哈希。同样的内容存多少次，哈希都一样（ADR-20）。
struct StoredFile {
    std::string sha256;
    long long bytes = 0;
};

class SampleContent {
public:
    virtual ~SampleContent() = default;
    virtual Result<StoredFile> importFile(const std::string& path) = 0;
    virtual std::string pathOf(const std::string& sha256) const = 0;  // 内容存在哪个文件里（读图像时用）
};

// ===== 查询用 DTO =====

struct SampleView {
    std::string id;                  // 图像内容的哈希
    std::vector<std::string> names;  // 导入时用过的文件名
    std::string label;               // unlabeled / ok / ng
    std::string use;                 // unassigned / benchmark / training
    std::string contentPath;         // 图像文件的位置
};

struct DatasetEntryView {
    std::string sampleId;
    std::string label;
    std::string contentPath;
};

// 发布语言：冻结的数据集版本（BC-9 → BC-12 模型训练）
struct DatasetVersionView {
    std::string id;  // "<数据集ID>@v1"
    std::string name;
    std::vector<DatasetEntryView> entries;
    int okCount = 0, ngCount = 0;
};

// ===== 应用服务 =====

class DataService {
public:
    DataService(SampleRepository& samples, DatasetRepository& datasets, SampleContent& content, EventPublisher& events);

    // 导入一幅图像成为样本，返回样本 ID。同样内容的图像（哪怕文件名不同）得到同一个样本。
    // label：ok / ng，或者空（先不标）。已经有标签的样本再次导入时，给了不同的标签会被拒绝。
    Result<std::string> importSample(const std::string& imagePath, const std::string& label = "");
    Status labelSample(const std::string& sampleId, const std::string& label);
    Result<SampleView> getSample(const std::string& sampleId) const;
    std::vector<std::string> listSamples() const;

    Result<std::string> createDataset(const std::string& name);
    Status addToDataset(const std::string& datasetId, const std::vector<std::string>& sampleIds);
    // 冻结：检查规则 N1，生成数据集版本，把其中的样本的用途定为“用于训练”。返回数据集版本 ID。
    Result<std::string> freezeDataset(const std::string& datasetId);
    Result<DatasetVersionView> getDatasetVersion(const std::string& datasetVersionId) const;

    // 给基准图集用（BC-4 冻结基准图集时调用）：把这些样本的用途定为“留作基准”。
    // 其中有已经用于训练的 → 整体拒绝，一个都不改（N1）。
    Status reserveForBenchmark(const std::vector<std::string>& sampleIds);
    // 这些样本里有没有留作基准的（提交训练任务时再检查一次，T-2）。返回留作基准的那些。
    std::vector<std::string> benchmarkSamplesAmong(const std::vector<std::string>& sampleIds) const;

private:
    SampleRepository& samples_;
    DatasetRepository& datasets_;
    SampleContent& content_;
    EventPublisher& events_;
    mutable std::mutex mutex_;
};

}  // namespace vision::data
