// 模板入门与宏：从“同样的代码写三遍”到函数模板、类模板，再到一行注册宏。
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ---------- 1. 函数模板：同一段逻辑适用于多种类型 ----------

template <typename T>
T clampTo(T value, T low, T high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// ---------- 2. 类模板：一个简化版的 Result<T> ----------

template <typename T>
class MiniResult {
public:
    static MiniResult success(T value) { return MiniResult(std::move(value), {}); }
    static MiniResult failure(std::string message) { return MiniResult(std::nullopt, std::move(message)); }

    bool ok() const { return value_.has_value(); }
    const T& value() const { return *value_; }
    const std::string& message() const { return message_; }

private:
    MiniResult(std::optional<T> value, std::string message) : value_(std::move(value)), message_(std::move(message)) {}
    std::optional<T> value_;
    std::string message_;
};

MiniResult<int> parseDigit(char c) {
    if (c < '0' || c > '9') return MiniResult<int>::failure(std::string("不是数字：") + c);
    return MiniResult<int>::success(c - '0');
}

// ---------- 3. 成员函数模板：按调用者指定的类型取值（对应项目里的 ctx.input<T>()）----------

class Settings {
public:
    void set(const std::string& key, std::string value) { values_[key] = std::move(value); }

    template <typename T>
    std::optional<T> get(const std::string& key) const {
        const auto it = values_.find(key);
        if (it == values_.end()) return std::nullopt;
        return convert<T>(it->second);
    }

private:
    // 模板特化：为具体的类型分别提供实现
    template <typename T>
    static std::optional<T> convert(const std::string& text);

    std::map<std::string, std::string> values_;
};

template <>
std::optional<int> Settings::convert<int>(const std::string& text) {
    try {
        return std::stoi(text);
    } catch (...) {
        return std::nullopt;
    }
}
template <>
std::optional<std::string> Settings::convert<std::string>(const std::string& text) { return text; }
template <>
std::optional<bool> Settings::convert<bool>(const std::string& text) {
    if (text == "true") return true;
    if (text == "false") return false;
    return std::nullopt;
}

// ---------- 4. 静态注册：项目里“一行注册一个算子”的最小重现 ----------

class Operator {
public:
    virtual ~Operator() = default;
    virtual std::string run(const std::string& input) = 0;
};

class Registry {
public:
    using Factory = std::function<std::unique_ptr<Operator>()>;

    // 函数内的静态变量：第一次调用时创建，保证在任何注册发生之前就已经存在
    static Registry& global() {
        static Registry instance;
        return instance;
    }

    bool add(const std::string& id, Factory factory) {
        return factories_.emplace(id, std::move(factory)).second;
    }
    std::unique_ptr<Operator> create(const std::string& id) const {
        const auto it = factories_.find(id);
        return it == factories_.end() ? nullptr : it->second();
    }
    std::vector<std::string> ids() const {
        std::vector<std::string> out;
        for (const auto& [id, factory] : factories_) out.push_back(id);
        return out;
    }

private:
    std::map<std::string, Factory> factories_;
};

// 宏：在预处理阶段做文本替换。## 把两段文字拼成一个标识符。
// 它定义了一个全局的 bool 变量，用“给它赋初值”的机会去调用 Registry::add。
// 全局变量在 main 开始之前初始化，所以算子在程序启动时就自动注册好了。
#define REGISTER_OPERATOR(OpClass, id)                                                                       \
    namespace {                                                                                              \
    [[maybe_unused]] const bool registered_##OpClass =                                                       \
        Registry::global().add(id, [] { return std::unique_ptr<Operator>(new OpClass()); });                 \
    }

class UpperOp final : public Operator {
public:
    std::string run(const std::string& input) override {
        std::string out = input;
        for (char& c : out) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return out;
    }
};
REGISTER_OPERATOR(UpperOp, "text.upper")

class ReverseOp final : public Operator {
public:
    std::string run(const std::string& input) override { return std::string(input.rbegin(), input.rend()); }
};
REGISTER_OPERATOR(ReverseOp, "text.reverse")

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

int main() {
    // 1. 函数模板：编译器根据参数类型自动生成对应的版本
    check(clampTo(300, 0, 255) == 255, "int 版本");
    check(clampTo(-0.5, 0.0, 1.0) == 0.0, "double 版本");
    check(clampTo(std::string("m"), std::string("a"), std::string("f")) == "f", "string 版本");
    check(clampTo<double>(3, 0.5, 2.5) == 2.5, "类型不一致时，明确指定 T");

    // 2. 类模板
    const auto d = parseDigit('7');
    check(d.ok() && d.value() == 7, "MiniResult<int> 成功");
    const auto e = parseDigit('x');
    check(!e.ok(), "MiniResult<int> 失败");
    std::cout << e.message() << "\n";
    const auto s = MiniResult<std::string>::success("同一个模板，换一种类型");
    check(s.ok(), "MiniResult<std::string>");

    // 3. 成员函数模板
    Settings settings;
    settings.set("threshold", "128");
    settings.set("invert", "true");
    settings.set("label", "亮斑");
    check(settings.get<int>("threshold") == 128, "get<int>");
    check(settings.get<bool>("invert") == true, "get<bool>");
    check(settings.get<std::string>("label") == std::string("亮斑"), "get<std::string>");
    check(!settings.get<int>("label").has_value(), "类型不对时返回空");
    check(!settings.get<int>("missing").has_value(), "键不存在时返回空");

    // 4. 静态注册：main 里没有任何注册代码，但两个算子已经在目录里了
    check(Registry::global().ids().size() == 2, "两个算子已自动注册");
    const auto upper = Registry::global().create("text.upper");
    check(upper != nullptr && upper->run("blob") == "BLOB", "创建并运行 text.upper");
    check(Registry::global().create("text.reverse")->run("abc") == "cba", "创建并运行 text.reverse");
    check(Registry::global().create("text.unknown") == nullptr, "没有注册的算子");

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
