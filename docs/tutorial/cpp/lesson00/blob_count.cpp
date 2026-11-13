// 工程写法 · 源文件：放“实现”。
#include "blob_count.h"

#include <array>
#include <queue>
#include <utility>

namespace lesson00 {

std::optional<BinaryGrid> BinaryGrid::fromLines(const std::vector<std::string>& lines) {
    if (lines.empty()) return BinaryGrid(0, 0, {});
    const std::size_t cols = lines.front().size();
    std::vector<bool> cells;
    cells.reserve(lines.size() * cols);
    for (const std::string& line : lines) {
        if (line.size() != cols) return std::nullopt;
        for (char c : line) cells.push_back(c == '1');
    }
    return BinaryGrid(lines.size(), cols, std::move(cells));
}

namespace {  // 匿名命名空间：里面的东西只在这个文件里可见

struct Cell {
    std::size_t row;
    std::size_t col;
};

// 把从 start 出发能走到的前景格子全部标记为已访问。
void markBlob(const BinaryGrid& grid, Cell start, std::vector<bool>& visited) {
    static constexpr std::array<std::pair<int, int>, 4> kNeighbors{{{0, 1}, {0, -1}, {1, 0}, {-1, 0}}};

    std::queue<Cell> pending;
    pending.push(start);
    visited[start.row * grid.cols() + start.col] = true;

    while (!pending.empty()) {
        const Cell current = pending.front();
        pending.pop();
        for (const auto& [dRow, dCol] : kNeighbors) {
            const long row = static_cast<long>(current.row) + dRow;
            const long col = static_cast<long>(current.col) + dCol;
            if (row < 0 || col < 0) continue;
            const Cell next{static_cast<std::size_t>(row), static_cast<std::size_t>(col)};
            if (next.row >= grid.rows() || next.col >= grid.cols()) continue;
            const std::size_t index = next.row * grid.cols() + next.col;
            if (!grid.at(next.row, next.col) || visited[index]) continue;
            visited[index] = true;
            pending.push(next);
        }
    }
}

}  // namespace

int countBlobs(const BinaryGrid& grid) {
    std::vector<bool> visited(grid.rows() * grid.cols(), false);
    int count = 0;
    for (std::size_t row = 0; row < grid.rows(); ++row) {
        for (std::size_t col = 0; col < grid.cols(); ++col) {
            if (grid.at(row, col) && !visited[row * grid.cols() + col]) {
                markBlob(grid, Cell{row, col}, visited);
                ++count;
            }
        }
    }
    return count;
}

}  // namespace lesson00
