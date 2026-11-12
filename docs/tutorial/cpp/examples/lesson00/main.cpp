// 工程写法 · 程序入口：只负责读输入、调用、输出。算法不在这里。
#include <iostream>
#include <string>
#include <vector>

#include "blob_count.h"

int main() {
    std::size_t rows = 0, cols = 0;
    if (!(std::cin >> rows >> cols)) {
        std::cerr << "输入格式错误：第一行应为“行数 列数”\n";
        return 1;
    }
    std::vector<std::string> lines(rows);
    for (std::string& line : lines) std::cin >> line;

    const auto grid = lesson00::BinaryGrid::fromLines(lines);
    if (!grid || grid->cols() != cols) {
        std::cerr << "输入格式错误：每一行的长度必须等于列数\n";
        return 1;
    }
    std::cout << lesson00::countBlobs(*grid) << "\n";
    return 0;
}
