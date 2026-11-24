#pragma once

#include <filesystem>
#include <string>

#include "vision/catalog/api.h"
#include "vision/content/content_store.h"
#include "vision/models/api.h"
#include "vision/release/api.h"

namespace vision::exchange {

// 方案包的文件格式：一个不压缩的 tar 文件（扩展名建议 .vpkg）。
//
//   manifest.json          清单：包里有什么，每一份内容的哈希
//   recipe-version.json    方案版本文档（发布语言）
//   content/<sha256>       匹配模板的图像、模型文件，文件名就是内容的哈希
//
// 为什么是 tar：格式简单到可以自己写（不用为它引入一个压缩库），又是标准格式——
// 用系统自带的 tar 命令就能看里面有什么：tar tf 某个包.vpkg
// 不压缩：里面主要是 PNG 和模型权重，本来就压不动。
//
// 读的时候核对：每一份内容的哈希、方案文档的哈希，都必须和清单一致。
class TarPackageExchange final : public release::PackageExchange {
public:
    Status write(const std::string& filePath, const release::PackageFile& package) override;
    Result<release::PackageFile> read(const std::string& filePath) override;
    std::string fingerprint(const authoring::RecipeVersionDocument& recipe) const override;
};

// ---- 把方案包需要的几个端口接到各个上下文上 ----

class CatalogOperatorInventory final : public release::OperatorInventory {
public:
    explicit CatalogOperatorInventory(const catalog::OperatorCatalog& catalog) : catalog_(catalog) {}
    std::optional<release::OperatorFacts> find(const std::string& operatorId) const override;

private:
    const catalog::OperatorCatalog& catalog_;
};

class RepositoryModelArchive final : public release::ModelArchive {
public:
    explicit RepositoryModelArchive(models::ModelService& models) : models_(models) {}
    Result<release::ArchivedModel> find(const std::string& modelVersionId) override;
    Status add(const release::PackageModelEntry& entry, const std::vector<unsigned char>& bytes) override;

private:
    models::ModelService& models_;
};

class StoreContentArchive final : public release::ContentArchive {
public:
    explicit StoreContentArchive(content::FileContentStore& store) : store_(store) {}
    Result<std::vector<unsigned char>> get(const std::string& sha256) override { return store_.get(sha256); }
    Status put(const std::vector<unsigned char>& bytes, const std::string& expectedSha256) override;

private:
    content::FileContentStore& store_;
};

}  // namespace vision::exchange
