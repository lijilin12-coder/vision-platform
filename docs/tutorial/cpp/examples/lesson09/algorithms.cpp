// lambda、std::function 与 STL 算法：用连通块（Blob）列表做例子。
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

struct Blob {
    int id;
    double area;
    double centerX;
    double centerY;
};

// 接收“一个判断条件”作为参数。std::function 可以装下任何签名匹配的可调用对象。
std::vector<Blob> filter(const std::vector<Blob>& blobs, const std::function<bool(const Blob&)>& keep) {
    std::vector<Blob> out;
    std::copy_if(blobs.begin(), blobs.end(), std::back_inserter(out), keep);
    return out;
}

// 返回一个 lambda。按值捕获 minArea：lambda 里保存了它的一份副本，函数返回后仍然有效。
std::function<bool(const Blob&)> largerThan(double minArea) {
    return [minArea](const Blob& b) { return b.area >= minArea; };
}

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

int main() {
    std::vector<Blob> blobs{{1, 120, 10, 10}, {2, 8, 50, 20}, {3, 300, 90, 40}, {4, 15, 30, 70}, {5, 64, 60, 60}};

    // ---- lambda 的三个部分：[捕获](参数) { 函数体 } ----
    const double minArea = 20;
    const auto isLarge = [minArea](const Blob& b) { return b.area >= minArea; };
    check(isLarge(blobs[0]) && !isLarge(blobs[1]), "lambda 可以像函数一样调用");

    // ---- 查询类算法 ----
    check(std::any_of(blobs.begin(), blobs.end(), isLarge), "any_of：至少有一个大的");
    check(!std::all_of(blobs.begin(), blobs.end(), isLarge), "all_of：不是全部都大");
    check(std::none_of(blobs.begin(), blobs.end(), [](const Blob& b) { return b.area < 0; }), "none_of：没有面积为负的");
    check(std::count_if(blobs.begin(), blobs.end(), isLarge) == 3, "count_if：有 3 个大的");

    const auto it = std::find_if(blobs.begin(), blobs.end(), [](const Blob& b) { return b.id == 3; });
    check(it != blobs.end() && it->area == 300, "find_if：找到 id 为 3 的");
    check(std::find_if(blobs.begin(), blobs.end(), [](const Blob& b) { return b.id == 99; }) == blobs.end(),
          "find_if：找不到时返回 end()");

    const auto largest = std::max_element(blobs.begin(), blobs.end(),
                                          [](const Blob& a, const Blob& b) { return a.area < b.area; });
    check(largest->id == 3, "max_element：面积最大的");

    // ---- 累加 ----
    const double total = std::accumulate(blobs.begin(), blobs.end(), 0.0,
                                         [](double sum, const Blob& b) { return sum + b.area; });
    check(total == 507, "accumulate：总面积");

    // ---- 排序：传入比较规则 ----
    std::vector<Blob> sorted = blobs;
    std::sort(sorted.begin(), sorted.end(), [](const Blob& a, const Blob& b) { return a.area > b.area; });
    check(sorted.front().id == 3 && sorted.back().id == 2, "sort：按面积从大到小");

    // ---- 变换 ----
    std::vector<int> ids;
    std::transform(sorted.begin(), sorted.end(), std::back_inserter(ids), [](const Blob& b) { return b.id; });
    check((ids == std::vector<int>{3, 1, 5, 4, 2}), "transform：取出每个 Blob 的 id");

    // ---- 删除：erase-remove 惯用法 ----
    std::vector<Blob> kept = blobs;
    kept.erase(std::remove_if(kept.begin(), kept.end(), [](const Blob& b) { return b.area < 20; }), kept.end());
    check(kept.size() == 3, "erase + remove_if：删掉面积小于 20 的");

    // ---- 把条件作为参数传递 ----
    check(filter(blobs, largerThan(100)).size() == 2, "把 lambda 传给函数");
    check(filter(blobs, [](const Blob& b) { return b.centerX > 40; }).size() == 3, "换一个条件，函数不用改");

    // ---- 按引用捕获：lambda 里直接修改外面的变量 ----
    int visited = 0;
    double areaSum = 0;
    std::for_each(blobs.begin(), blobs.end(), [&visited, &areaSum](const Blob& b) {
        ++visited;
        areaSum += b.area;
    });
    check(visited == 5 && areaSum == 507, "按引用捕获");

    // ---- 按值捕获的是“创建 lambda 那一刻”的值 ----
    double threshold = 100;
    const auto byValue = [threshold](const Blob& b) { return b.area >= threshold; };
    const auto byRef = [&threshold](const Blob& b) { return b.area >= threshold; };
    threshold = 10;  // 之后修改
    check(std::count_if(blobs.begin(), blobs.end(), byValue) == 2, "按值捕获：仍然用 100");
    check(std::count_if(blobs.begin(), blobs.end(), byRef) == 4, "按引用捕获：用的是修改后的 10");

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
