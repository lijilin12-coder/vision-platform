// 对外的头文件：使用者能看到的全部内容。
#pragma once

namespace geometry {

// 矩形：左上角 (x, y)，宽 width，高 height。
struct Rect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

// 面积。宽或高不是正数时，面积为 0。
double area(const Rect& rect);

// 两个矩形的重叠部分。不重叠时，返回宽高都为 0 的矩形。
Rect intersection(const Rect& a, const Rect& b);

// 重叠度（交并比，IoU）：重叠面积 ÷ 合并面积，范围 0～1。
// 视觉里用它衡量两个框有多接近。本项目的一致性标准里“缺陷位置重叠度 ≥ 0.9”说的就是它。
double overlapRatio(const Rect& a, const Rect& b);

}  // namespace geometry
