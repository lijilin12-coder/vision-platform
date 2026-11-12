#include "geometry/rect.h"

#include <algorithm>

#include "detail.h"

namespace geometry {

double area(const Rect& rect) { return detail::clampToZero(rect.width) * detail::clampToZero(rect.height); }

Rect intersection(const Rect& a, const Rect& b) {
    const double left = std::max(a.x, b.x);
    const double top = std::max(a.y, b.y);
    const double right = std::min(a.x + a.width, b.x + b.width);
    const double bottom = std::min(a.y + a.height, b.y + b.height);
    if (right <= left || bottom <= top) return Rect{};
    return Rect{left, top, right - left, bottom - top};
}

}  // namespace geometry
