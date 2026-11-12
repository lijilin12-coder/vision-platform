#include <iostream>

#include "geometry/rect.h"
// #include "detail.h"   // 取消这行注释会编译失败：detail.h 在库的 PRIVATE 目录里，这里找不到

int main() {
    const geometry::Rect expected{100, 100, 50, 40};  // 期望的缺陷位置
    const geometry::Rect detected{105, 102, 50, 40};  // 实际检测到的位置

    std::cout << "重叠度：" << geometry::overlapRatio(expected, detected) << "\n";
    return 0;
}
