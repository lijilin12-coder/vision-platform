#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vision/kernel/geometry.h"

namespace vision {

// 结果项：一次检测“发现了什么、量出了什么、零件在哪里”（术语表 §6，docs/ddd/10-result-items.md）。
// 它们和判定依据不是一回事：判定依据回答“为什么合格 / 不合格”，是给人看的一句话；
// 结果项是带结构的数据，给下游用——画在图像上、做统计、和基准比较。
//
// 结果项按类型区分（缺陷、测量值、位姿是三种不同的东西，下游的用途不同）。
// 由“知道含义”的算子上报：Blob 分析只知道那是一些连通块，个数判定才知道它们是“缺陷”。

// 缺陷：检测中发现的一个具体问题。位置和大小的单位是像素，坐标是【这个算子看到的图像】上的坐标
// （图像经过“按位姿对齐”之后，坐标是对齐后的，不是相机原图上的）。
struct Defect {
    std::string nodeId;    // 哪个节点上报的（由检测引擎填写，算子不用管）
    std::string category;  // 类别，例如“亮斑”“划痕”
    double centerX = 0, centerY = 0;
    int left = 0, top = 0, width = 0, height = 0;  // 外接矩形
    double area = 0;
};

// 测量值：某个测量项这一次的实测值。
struct Measurement {
    std::string nodeId;
    std::string name;  // 测量项的名称，例如“宽度”。同名的测量值可以跨检测放在一起做趋势统计
    double value = 0;
    std::string unit;  // 例如“像素”；可以为空
    std::optional<double> lower, upper;  // 允许的范围（有的话）
    bool pass = true;                    // 在不在允许的范围里
};

// 位姿结果：定位的结果。
struct PoseResult {
    std::string nodeId;
    Pose pose;
    bool found = true;  // 没找到时 pose 是基准位置，不能当真
    double score = 0;   // 相似程度，0～1
};

struct ResultItems {
    std::vector<Defect> defects;
    std::vector<Measurement> measurements;
    std::vector<PoseResult> poses;

    bool empty() const { return defects.empty() && measurements.empty() && poses.empty(); }
};

}  // namespace vision
