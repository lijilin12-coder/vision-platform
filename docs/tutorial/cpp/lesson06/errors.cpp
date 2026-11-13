// 错误处理的三种方式：optional、Result、异常。
// 这个示例直接使用项目里的 Result（core/common/include/vision/common/result.h）。
// 编译时需要加上头文件目录，见教程第 6 课第 7 节。
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "vision/common/result.h"

using vision::Error;
using vision::Result;
using vision::Status;

// ---------- 1. optional：“没有”是正常情况，不需要解释原因 ----------

std::optional<double> findParam(const std::map<std::string, double>& params, const std::string& name) {
    const auto it = params.find(name);
    if (it == params.end()) return std::nullopt;
    return it->second;
}

// ---------- 2. Result：预料之中的失败，调用者需要知道原因 ----------

struct PortRef {
    std::string node;
    std::string port;
};

// 解析 "n1.image" 这样的端口引用。格式不对是用户输入的问题，属于预料之中的失败。
Result<PortRef> parsePortRef(const std::string& text) {
    const auto dot = text.find('.');
    if (dot == std::string::npos) {
        return Result<PortRef>::failure("port_ref.no_dot", "端口引用必须形如 n1.image：" + text);
    }
    if (dot == 0 || dot + 1 == text.size()) {
        return Result<PortRef>::failure("port_ref.empty_part", "节点名和端口名都不能为空：" + text);
    }
    return PortRef{text.substr(0, dot), text.substr(dot + 1)};
}

// 没有返回值的操作用 Status（即 Result<void>）
Status checkThreshold(double value) {
    if (value < 0 || value > 255) {
        return Status::failure("param.out_of_range", "阈值必须在 0～255 之间，实际为 " + std::to_string(value));
    }
    return Status::success();
}

// 错误向上传递：每一步失败就原样返回
Result<std::string> describeEdge(const std::string& from, const std::string& to) {
    auto f = parsePortRef(from);
    if (!f) return f.error();
    auto t = parsePortRef(to);
    if (!t) return t.error();
    return f.value().node + " 的 " + f.value().port + " → " + t.value().node + " 的 " + t.value().port;
}

// ---------- 3. 异常：不该发生的情况；构造函数里的失败 ----------

class Matrix {
public:
    Matrix(int rows, int cols) : rows_(rows), cols_(cols) {
        // 构造函数没有返回值，报告失败只能靠异常
        if (rows <= 0 || cols <= 0) throw std::invalid_argument("矩阵的行数和列数必须为正");
        data_.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols), 0.0);
    }
    double at(int r, int c) const {
        // 越界是调用者的程序写错了，不是可以预料的业务情况
        if (r < 0 || r >= rows_ || c < 0 || c >= cols_) throw std::out_of_range("Matrix::at 下标越界");
        return data_[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)];
    }

private:
    int rows_;
    int cols_;
    std::vector<double> data_;
};

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

int main() {
    // 1. optional
    const std::map<std::string, double> params{{"value", 128.0}};
    check(findParam(params, "value").has_value(), "找到参数");
    check(!findParam(params, "missing"), "找不到参数");
    check(findParam(params, "missing").value_or(0.0) == 0.0, "value_or 提供默认值");
    if (const auto v = findParam(params, "value")) std::cout << "value = " << *v << "\n";

    // 2. Result
    const auto good = parsePortRef("n1.image");
    check(good.ok() && good.value().node == "n1" && good.value().port == "image", "解析成功");

    const auto bad = parsePortRef("n1image");
    check(!bad.ok() && bad.error().code == "port_ref.no_dot", "解析失败，带错误码");
    std::cout << "错误：" << bad.error().message << "（" << bad.error().code << "）\n";

    check(checkThreshold(128).ok(), "阈值合法");
    check(!checkThreshold(300).ok(), "阈值超出范围");

    check(describeEdge("n1.image", "n2.image").ok(), "两端都合法");
    const auto edge = describeEdge("n1.image", ".image");
    check(!edge.ok() && edge.error().code == "port_ref.empty_part", "第二步的错误被传了上来");

    // 对失败的 Result 调用 value() 是程序写错了：会抛出 std::logic_error
    bool threw = false;
    try {
        (void)bad.value();
    } catch (const std::logic_error&) {
        threw = true;
    }
    check(threw, "对失败的 Result 取值会抛异常");

    // 3. 异常
    threw = false;
    try {
        Matrix m(0, 3);
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::cout << "捕获异常：" << e.what() << "\n";
    }
    check(threw, "非法的构造参数抛出异常");

    threw = false;
    try {
        const Matrix m(2, 2);
        (void)m.at(5, 5);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    check(threw, "越界访问抛出异常");

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
