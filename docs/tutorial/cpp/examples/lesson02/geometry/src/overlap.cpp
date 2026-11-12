#include "detail.h"
#include "geometry/rect.h"

namespace geometry {

namespace {  // 匿名命名空间：这个函数只在本文件内可见，别的文件里可以有同名函数而不冲突

double unionArea(const Rect& a, const Rect& b) { return area(a) + area(b) - area(intersection(a, b)); }

}  // namespace

double overlapRatio(const Rect& a, const Rect& b) {
    const double total = unionArea(a, b);
    if (total <= 0) return 0;
    return area(intersection(a, b)) / total;
}

}  // namespace geometry
