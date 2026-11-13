// 内部头文件：只给 geometry 库自己的源文件使用，使用者包含不到它。
#pragma once

namespace geometry::detail {

// inline：函数定义写在头文件里时必须加，否则被两个源文件包含后会“重复定义”。
inline double clampToZero(double value) { return value > 0 ? value : 0; }

}  // namespace geometry::detail
