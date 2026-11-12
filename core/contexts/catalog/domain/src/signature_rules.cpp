#include "vision/catalog/domain/signature_rules.h"

#include <set>

namespace vision::catalog {

namespace {
template <typename T>
bool hasDuplicateNames(const std::vector<T>& items) {
    std::set<std::string> seen;
    for (const auto& i : items) {
        if (!seen.insert(i.name).second) return true;
    }
    return false;
}
}  // namespace

Status validateSignature(const OperatorSignature& s) {
    auto fail = [&](const std::string& why) {
        return Status::failure("catalog.invalid_signature", "算子 " + s.id + "：" + why);
    };
    if (s.id.empty() || s.id.find('.') == std::string::npos) return fail("ID 必须形如“分类.名称”");
    if (s.majorVersion < 1) return fail("主版本号必须 >= 1");
    if (hasDuplicateNames(s.inputs)) return fail("输入端口重名");
    if (hasDuplicateNames(s.outputs)) return fail("输出端口重名");
    if (hasDuplicateNames(s.params)) return fail("参数重名");
    for (const auto& p : s.params) {
        if (p.type == ParamType::Choice && p.choices.empty()) return fail("选项参数 " + p.name + " 没有选项");
        if (auto st = p.validate(p.defaultValue); !st) return fail("默认值不合法：" + st.error().message);
    }
    return Status::success();
}

}  // namespace vision::catalog
