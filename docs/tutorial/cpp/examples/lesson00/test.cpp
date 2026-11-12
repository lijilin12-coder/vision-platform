// 工程写法 · 测试：因为 countBlobs 不依赖全局变量和 cin，可以直接用几组数据检查它。
// （第 10 课会改用 GoogleTest，这里先用最朴素的 assert。）
#include <cassert>
#include <iostream>

#include "blob_count.h"

using lesson00::BinaryGrid;
using lesson00::countBlobs;

int main() {
    assert(countBlobs(*BinaryGrid::fromLines({})) == 0);
    assert(countBlobs(*BinaryGrid::fromLines({"000", "000"})) == 0);
    assert(countBlobs(*BinaryGrid::fromLines({"111", "111"})) == 1);
    assert(countBlobs(*BinaryGrid::fromLines({"101", "010", "101"})) == 5);  // 4 邻域：斜着不算相连
    assert(countBlobs(*BinaryGrid::fromLines({"1100", "0100", "0011"})) == 2);

    // 同一个函数可以连续调用多次，结果互不影响（竞赛写法里要记得清空 vis 和 ans）
    const auto grid = *BinaryGrid::fromLines({"10", "01"});
    assert(countBlobs(grid) == 2);
    assert(countBlobs(grid) == 2);

    // 各行长度不一致：创建失败
    assert(!BinaryGrid::fromLines({"11", "1"}).has_value());

    std::cout << "全部通过\n";
    return 0;
}
