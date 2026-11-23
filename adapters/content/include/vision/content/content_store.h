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
#include "vision/execution/api.h"

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

}  // namespace vision::content
