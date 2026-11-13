// 本项目里真实出现过的 bug 的最小重现：对象被 move 走之后，又通过引用使用了它的内容。
// 原始位置：core/contexts/catalog/application/src/operator_catalog.cpp
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <utility>

struct OperatorMeta {
    std::string id;
    std::string displayName;
};

struct Entry {
    OperatorMeta meta;
};

class BuggyRegistry {
public:
    void add(OperatorMeta meta) {
        const auto& id = meta.id;                     // id 是一个引用：它只是 meta.id 的另一个名字
        entries_.emplace(id, Entry{std::move(meta)}); // ← 问题在这一行
        // 这一行里，编译器先构造 Entry{std::move(meta)}：meta 的内容被搬进了 Entry，meta.id 变成空字符串。
        // 然后才用 id 构造 map 的键。可是 id 就是 meta.id，这时它已经是空的了。
        // 结果：每个算子都以 "" 为键。第一个放进去了，后面的因为键重复，全部被丢弃。
    }
    std::size_t size() const { return entries_.size(); }
    bool has(const std::string& id) const { return entries_.count(id) != 0; }

private:
    std::map<std::string, Entry> entries_;
};

class FixedRegistry {
public:
    void add(OperatorMeta meta) {
        const std::string id = meta.id;               // 修复：复制一份。id 现在是一个独立的字符串
        entries_.emplace(id, Entry{std::move(meta)});
    }
    std::size_t size() const { return entries_.size(); }
    bool has(const std::string& id) const { return entries_.count(id) != 0; }

private:
    std::map<std::string, Entry> entries_;
};

template <typename Registry>
void registerThree(Registry& registry) {
    registry.add({"vision.to_gray", "灰度"});
    registry.add({"vision.threshold", "阈值"});
    registry.add({"vision.blob", "Blob 分析"});
}

int main() {
    BuggyRegistry buggy;
    registerThree(buggy);
    std::cout << "有 bug 的版本：注册了 3 个算子，目录里有 " << buggy.size() << " 个；"
              << "能找到 vision.threshold 吗？" << (buggy.has("vision.threshold") ? "能" : "不能") << "\n";

    FixedRegistry fixed;
    registerThree(fixed);
    std::cout << "修复后的版本：注册了 3 个算子，目录里有 " << fixed.size() << " 个；"
              << "能找到 vision.threshold 吗？" << (fixed.has("vision.threshold") ? "能" : "不能") << "\n";

    // 只对修复后的版本做检查。有 bug 的版本读取的是“被移走的对象”，
    // 标准只保证它处于某个合法状态，并不保证一定是空字符串。
    return fixed.size() == 3 && fixed.has("vision.threshold") ? EXIT_SUCCESS : EXIT_FAILURE;
}
