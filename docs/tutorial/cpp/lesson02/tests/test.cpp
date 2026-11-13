#include <cmath>
#include <cstdlib>
#include <iostream>

#include "geometry/rect.h"

using geometry::Rect;

namespace {

int failures = 0;

// 不用 assert：Release 模式会定义 NDEBUG，assert 会被整个去掉，测试就形同虚设。
void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "失败：" << what << "\n";
        ++failures;
    }
}

bool nearlyEqual(double a, double b) { return std::fabs(a - b) < 1e-9; }

}  // namespace

int main() {
    check(nearlyEqual(geometry::area(Rect{0, 0, 4, 5}), 20), "面积");
    check(nearlyEqual(geometry::area(Rect{0, 0, -4, 5}), 0), "宽为负数时面积为 0");

    const Rect overlap = geometry::intersection(Rect{0, 0, 10, 10}, Rect{5, 5, 10, 10});
    check(nearlyEqual(overlap.x, 5) && nearlyEqual(overlap.width, 5) && nearlyEqual(overlap.height, 5), "交集");

    check(nearlyEqual(geometry::overlapRatio(Rect{0, 0, 10, 10}, Rect{0, 0, 10, 10}), 1), "完全重合时重叠度为 1");
    check(nearlyEqual(geometry::overlapRatio(Rect{0, 0, 10, 10}, Rect{20, 20, 10, 10}), 0), "不相交时重叠度为 0");
    check(nearlyEqual(geometry::overlapRatio(Rect{0, 0, 10, 10}, Rect{5, 0, 10, 10}), 50.0 / 150.0), "一半重叠");
    check(nearlyEqual(geometry::overlapRatio(Rect{}, Rect{}), 0), "两个空矩形");

    if (failures == 0) std::cout << "全部通过\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
