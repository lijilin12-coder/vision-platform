#pragma once

// 内容寻址存储（ADR-20）：文件名就是内容的 SHA-256。
//   - 同样的内容只存一份，写入是幂等的
//   - 内容不可修改：改了内容，哈希就变了，就是另一个东西
//   - 读出来之后校验哈希：文件被改过或者损坏，读取失败，而不是悄悄交出错的内容

#include <filesystem>
#include <string>
#include <vector>

#include "vision/authoring/api.h"
#include "vision/common/result.h"
#include "vision/data/api.h"
#include "vision/execution/api.h"
#include "vision/models/api.h"
#include "vision/quality/api.h"
#include "vision/release/api.h"
#include "vision/training/api.h"
#include "vision/training/domain/training_job.h"

namespace vision::content {

// 内容的 SHA-256，小写十六进制，64 个字符
std::string sha256Hex(const unsigned char* data, std::size_t size);
inline std::string sha256Hex(const std::vector<unsigned char>& data) { return sha256Hex(data.data(), data.size()); }

// <root>/content/sha256/<前两位>/<其余 62 位>
class FileContentStore {
public:
    explicit FileContentStore(std::filesystem::path root);
    Result<std::string> put(const std::vector<unsigned char>& bytes);            // 返回哈希
    Result<std::vector<unsigned char>> get(const std::string& sha256) const;     // 校验哈希
    bool contains(const std::string& sha256) const;
    std::filesystem::path pathOf(const std::string& sha256) const;

private:
    std::filesystem::path dir_;
};

// 匹配模板的存取：实现方案编排的 TemplateStore 和检测执行的 TemplateSource 两个端口。
// 模板保存为 PNG（无损）；哈希对 PNG 文件的字节计算。
class TemplateContent final : public authoring::TemplateStore, public execution::TemplateSource {
public:
    explicit TemplateContent(FileContentStore& store) : store_(store) {}
    Result<authoring::StoredContent> storeCrop(const Image& source, int x, int y, int width, int height) override;
    Result<Image> load(const authoring::RecipeDocumentTemplate& ref) override;

private:
    FileContentStore& store_;
};

// 样本的图像内容：把导入的图像文件原样存进内容仓库。实现数据上下文的 SampleContent 端口。
// 哈希对文件的字节计算：同一个文件换个名字再导入，哈希相同，就是同一个样本。
// （同一幅画面重新保存成另一种格式，字节不同，会被当成两个样本——见 docs/06-open-questions.md Q-09。）
// 检测时的图像（docs/ddd/18-image-cache.md）。不放进内容仓库：内容仓库里的东西可能被好几处引用，没法安全地删；
// 而检测图像的大多数是要过期清掉的。所以单独放，按工位、按日期分目录——一整天的可以一起删。
//
//   <root>/inspection-images/<工位 ID>/kept/<记录编号>.png            长期保留
//   <root>/inspection-images/<工位 ID>/cache/<日期>/<记录编号>.png    短期缓存
//
// 图像的编号就是检测记录的编号（由它能算出日期，所以不用另外记图像在哪个目录）。
// 以前的版本把检测图像存在内容仓库里，编号是内容哈希：那些照旧能读，算作长期保留。
class InspectionImageFiles final : public quality::ImageArchive {
public:
    InspectionImageFiles(const std::filesystem::path& root, FileContentStore& legacy);
    Result<std::string> store(const std::string& stationId, const std::string& recordId, const Image& image, bool longTerm) override;
    std::string state(const std::string& stationId, const std::string& imageId) override;
    Result<std::vector<unsigned char>> load(const std::string& stationId, const std::string& imageId) override;
    Status keep(const std::string& stationId, const std::string& imageId) override;
    quality::ImageCleanup cleanup(std::int64_t cutoffMs, std::uint64_t minFreeBytes) override;

private:
    std::filesystem::path keptFile(const std::string& stationId, const std::string& imageId) const;
    std::filesystem::path cachedFile(const std::string& stationId, const std::string& imageId) const;
    std::filesystem::path dir_;
    FileContentStore& legacy_;
};

class SampleFiles final : public data::SampleContent {
public:
    explicit SampleFiles(FileContentStore& store) : store_(store) {}
    Result<data::StoredFile> importFile(const std::string& path) override;
    std::string pathOf(const std::string& sha256) const override;

private:
    FileContentStore& store_;
};

// 把上线管理的“样本登记处”端口接到数据上下文上：基准图集的用例由此变成对样本的引用。
class DataSampleRegistry final : public release::SampleRegistry {
public:
    explicit DataSampleRegistry(data::DataService& data) : data_(data) {}
    Result<std::string> sampleOf(const std::string& imageRef, bool truthNg) override {
        return data_.importSample(imageRef, truthNg ? "ng" : "ok");
    }
    Status reserveForBenchmark(const std::vector<std::string>& sampleIds) override { return data_.reserveForBenchmark(sampleIds); }

private:
    data::DataService& data_;
};

// ---------- 训练闭环的几个接缝（POC-13）----------

// 模型文件进内容仓库。实现模型仓库的 ModelFiles 端口。
class ModelFileStore final : public models::ModelFiles {
public:
    explicit ModelFileStore(FileContentStore& store) : store_(store) {}
    Result<models::StoredModelFile> importFile(const std::string& path) override;
    Result<models::StoredModelFile> importBytes(const std::vector<unsigned char>& bytes) override;
    std::string pathOf(const std::string& sha256) const override;

private:
    FileContentStore& store_;
};

// 训练要用的数据集版本 → 数据上下文
class DataDatasetSource final : public training::DatasetSource {
public:
    explicit DataDatasetSource(const data::DataService& data) : data_(data) {}
    Result<training::DatasetInfo> load(const std::string& datasetVersionId) override;

private:
    const data::DataService& data_;
};

// 训练出来的模型 → 模型仓库
class RepositoryModelImporter final : public training::ModelImporter {
public:
    explicit RepositoryModelImporter(models::ModelService& models) : models_(models) {}
    Result<std::string> importTrained(const training::TrainedModel& model) override;

private:
    models::ModelService& models_;
};

// 没有配置训练后端时用的占位实现：任何训练都无法开始，并且说明原因。
// 这样没有训练服务的机器上，其余功能照常可用；提交的训练任务会以“已失败”记录下来。
class NoTrainingBackend final : public training::TrainingBackend {
public:
    std::string name() const override { return "none"; }
    Result<std::string> start(const training::TrainingRequest&) override {
        return Result<std::string>::failure("training.no_backend", "这台机器没有配置训练服务");
    }
    Result<training::BackendProgress> poll(const std::string&) override {
        return Result<training::BackendProgress>::failure("training.no_backend", "这台机器没有配置训练服务");
    }
    Status cancel(const std::string&) override { return Status::success(); }
};

}  // namespace vision::content
