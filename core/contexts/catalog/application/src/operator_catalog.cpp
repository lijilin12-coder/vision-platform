#include "vision/catalog/api.h"
#include "vision/catalog/domain/signature_rules.h"

namespace vision::catalog {

OperatorCatalog& OperatorCatalog::global() {
    static OperatorCatalog instance;
    return instance;
}

Status OperatorCatalog::registerOperator(OperatorMeta meta, OperatorFactory factory) {
    if (auto st = validateSignature(meta.signature); !st) return st;
    // 注意：必须复制一份 ID。如果用引用，下面 std::move(meta) 之后它会变成空字符串。
    const std::string id = meta.signature.id;
    if (entries_.count(id) != 0) {
        return Status::failure("catalog.duplicate_id", "算子 ID 冲突：" + id);
    }
    if (!factory) return Status::failure("catalog.no_factory", "算子 " + id + " 没有提供创建方法");
    entries_.emplace(id, Entry{std::move(meta), std::move(factory)});
    return Status::success();
}

bool OperatorCatalog::registerStatic(OperatorMeta meta, OperatorFactory factory) {
    auto st = registerOperator(std::move(meta), std::move(factory));
    if (!st) registrationErrors_.push_back(st.error());
    return st.ok();
}

std::vector<OperatorMeta> OperatorCatalog::list() const {
    std::vector<OperatorMeta> out;
    out.reserve(entries_.size());
    for (const auto& [id, e] : entries_) out.push_back(e.meta);
    return out;
}

std::optional<OperatorMeta> OperatorCatalog::find(const std::string& id) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) return std::nullopt;
    return it->second.meta;
}

Result<std::unique_ptr<Operator>> OperatorCatalog::create(const std::string& id, int majorVersion) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return Result<std::unique_ptr<Operator>>::failure("catalog.not_found", "找不到算子：" + id);
    }
    if (it->second.meta.signature.majorVersion != majorVersion) {
        return Result<std::unique_ptr<Operator>>::failure(
            "catalog.major_mismatch", "算子 " + id + " 的主版本号不匹配：需要 v" + std::to_string(majorVersion) +
                                          "，本平台为 v" + std::to_string(it->second.meta.signature.majorVersion));
    }
    return it->second.factory();
}

}  // namespace vision::catalog
