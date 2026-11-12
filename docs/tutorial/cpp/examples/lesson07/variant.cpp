// variant：一个变量在几种类型中“必居其一”。用多种形状的 ROI 做例子。
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

struct RectRoi {
    double x, y, width, height;
};
struct CircleRoi {
    double centerX, centerY, radius;
};
struct RingRoi {
    double centerX, centerY, innerRadius, outerRadius;
};

// Roi 是这三种形状之一。任何时刻，它里面只有其中一种。
using Roi = std::variant<RectRoi, CircleRoi, RingRoi>;

const double kPi = 3.14159265358979323846;

// ---------- 写法一：std::visit + 重载的函数对象 ----------

struct AreaVisitor {
    double operator()(const RectRoi& r) const { return r.width * r.height; }
    double operator()(const CircleRoi& c) const { return kPi * c.radius * c.radius; }
    double operator()(const RingRoi& r) const {
        return kPi * (r.outerRadius * r.outerRadius - r.innerRadius * r.innerRadius);
    }
};

double area(const Roi& roi) { return std::visit(AreaVisitor{}, roi); }

// ---------- 写法二：std::visit + 一组 lambda（需要下面这个两行的小工具）----------

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::string describe(const Roi& roi) {
    return std::visit(Overloaded{
                          [](const RectRoi& r) { return "矩形 " + std::to_string(int(r.width)) + "×" + std::to_string(int(r.height)); },
                          [](const CircleRoi& c) { return "圆 半径 " + std::to_string(int(c.radius)); },
                          [](const RingRoi& r) {
                              return "圆环 " + std::to_string(int(r.innerRadius)) + "～" + std::to_string(int(r.outerRadius));
                          },
                      },
                      roi);
}

// ---------- 写法三：只关心其中一种时，用 get_if ----------

bool isLargeCircle(const Roi& roi) {
    if (const auto* circle = std::get_if<CircleRoi>(&roi)) return circle->radius > 100;
    return false;
}

// ---------- 结构化绑定 ----------

// 一次返回多个值
std::tuple<double, double, int> statistics(const std::vector<Roi>& rois) {
    double total = 0, largest = 0;
    for (const auto& roi : rois) {
        const double a = area(roi);
        total += a;
        if (a > largest) largest = a;
    }
    return {total, largest, static_cast<int>(rois.size())};
}

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

int main() {
    const std::vector<Roi> rois{RectRoi{0, 0, 40, 30}, CircleRoi{100, 100, 10}, RingRoi{0, 0, 10, 20}};

    for (const auto& roi : rois) std::cout << describe(roi) << "，面积 " << area(roi) << "\n";

    check(area(rois[0]) == 1200, "矩形面积");
    check(std::fabs(area(rois[1]) - 100 * kPi) < 1e-9, "圆面积");
    check(std::fabs(area(rois[2]) - 300 * kPi) < 1e-9, "圆环面积");

    // 查询当前是哪一种
    check(std::holds_alternative<RectRoi>(rois[0]), "第一个是矩形");
    check(rois[1].index() == 1, "index() 返回类型在列表中的位置");
    check(!isLargeCircle(rois[1]) && isLargeCircle(CircleRoi{0, 0, 150}), "get_if");

    // std::get 拿错类型会抛异常
    bool threw = false;
    try {
        (void)std::get<CircleRoi>(rois[0]);
    } catch (const std::bad_variant_access&) {
        threw = true;
    }
    check(threw, "std::get 类型不对时抛出异常");

    // 结构化绑定：把 tuple、pair、结构体一次拆成几个变量
    const auto [total, largest, count] = statistics(rois);
    check(count == 3 && largest == 1200, "结构化绑定拆 tuple");
    std::cout << "共 " << count << " 个，总面积 " << total << "\n";

    const std::map<std::string, int> defectCounts{{"划痕", 3}, {"亮斑", 1}};
    int sum = 0;
    for (const auto& [name, n] : defectCounts) {  // 遍历 map 时拆开键和值
        std::cout << name << "：" << n << " 处\n";
        sum += n;
    }
    check(sum == 4, "结构化绑定遍历 map");

    // if 里带初始化语句：变量只在这个 if 里有效
    if (const auto it = defectCounts.find("划痕"); it != defectCounts.end()) {
        check(it->second == 3, "if 带初始化语句");
    } else {
        check(false, "应该能找到划痕");
    }

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
