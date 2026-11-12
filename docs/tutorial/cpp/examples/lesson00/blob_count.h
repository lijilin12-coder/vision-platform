// 工程写法 · 头文件：只放“声明”，告诉别人这个模块能做什么。
#pragma once  // 防止同一个头文件被重复包含

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lesson00 {

// 二值网格：true 表示前景。
// 用一个类把“数据”和“数据必须满足的规则”放在一起：每一行必须一样长。
class BinaryGrid {
public:
    // 从文本行创建，'1' 是前景，其他字符是背景。
    // 各行长度不一致时返回空（std::nullopt），而不是带着错误数据继续运行。
    static std::optional<BinaryGrid> fromLines(const std::vector<std::string>& lines);

    std::size_t rows() const { return rows_; }
    std::size_t cols() const { return cols_; }
    bool at(std::size_t row, std::size_t col) const { return cells_[row * cols_ + col]; }

private:
    BinaryGrid(std::size_t rows, std::size_t cols, std::vector<bool> cells)
        : rows_(rows), cols_(cols), cells_(std::move(cells)) {}

    std::size_t rows_;
    std::size_t cols_;
    std::vector<bool> cells_;  // 按行存放，大小由输入决定，不需要猜一个 N = 1005
};

// 数连通块（4 邻域）。
// 输入从参数进来，结果从返回值出去：不读写任何全局变量，所以可以反复调用、可以测试。
int countBlobs(const BinaryGrid& grid);

}  // namespace lesson00
