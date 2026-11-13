// 调试练习：这个程序有一个逻辑错误。它能编译，不崩溃，内存检查也不报错，只是结果不对。
// 任务：用调试器找出原因。（答案在教程第 12 课第 4 节。）
#include <iostream>
#include <queue>
#include <string>
#include <utility>
#include <vector>

// 返回网格里最大的连通块的面积（4 邻域，'1' 是前景）。
int largestBlobArea(const std::vector<std::string>& grid) {
    const int rows = static_cast<int>(grid.size());
    const int cols = rows == 0 ? 0 : static_cast<int>(grid[0].size());
    std::vector<std::vector<bool>> visited(rows, std::vector<bool>(cols, false));
    const int dRow[] = {0, 0, 1, -1};
    const int dCol[] = {1, -1, 0, 0};

    int largest = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (grid[r][c] != '1' || visited[r][c]) continue;

            int area = 0;
            std::queue<std::pair<int, int>> pending;
            pending.push({r, c});
            visited[r][c] = true;
            while (!pending.empty()) {
                const auto [curRow, curCol] = pending.front();
                pending.pop();
                ++area;
                for (int i = 0; i < 4; ++i) {
                    const int nextRow = curRow + dRow[i];
                    const int nextCol = curCol + dCol[i];
                    if (nextRow < 0 || nextRow >= rows || nextCol < 0 || nextCol >= rows) continue;
                    if (grid[nextRow][nextCol] != '1' || visited[nextRow][nextCol]) continue;
                    visited[nextRow][nextCol] = true;
                    pending.push({nextRow, nextCol});
                }
            }
            if (area > largest) largest = area;
        }
    }
    return largest;
}

int main() {
    const std::vector<std::string> square{"110", "110", "000"};
    const std::vector<std::string> wide{"111111", "000000"};

    std::cout << "3×3 网格：最大连通块面积 = " << largestBlobArea(square) << "（应为 4）\n";
    std::cout << "2×6 网格：最大连通块面积 = " << largestBlobArea(wide) << "（应为 6）\n";
    return 0;
}
