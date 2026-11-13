# 第 7 课：`variant`、`visit` 与结构化绑定

> 预计用时：1 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

方案里，节点之间的连线上流动着各种数据：图像、区域、Blob 列表、数值、判定依据。执行引擎要用**同一种变量**装下它们中的任何一种。

1. 一个变量怎么表示“几种类型中的某一种”？
2. 拿到这样的变量后，怎么按它实际的类型分别处理？
3. 顺带学两个让代码更简洁的写法：结构化绑定、带初始化的 `if`。

## 1. 问题：ROI 有好几种形状

ROI 可以是矩形、圆、圆环。三种形状需要的数据不一样。竞赛里常见的做法是：

```cpp
struct Roi {
    int type;                 // 0 = 矩形，1 = 圆，2 = 圆环
    double x, y, w, h;        // 矩形用
    double cx, cy, r;         // 圆用
    double r1, r2;            // 圆环用
};
```

问题很明显：

- 任何时候都有一大半成员是没有意义的，而且没有东西阻止你去读它们。
- `type` 和实际填写的数据可能对不上。
- 新增一种形状时，所有 `if (type == ...)` 的地方都要改，漏掉一处不会有任何提示。

## 2. `std::variant`：必居其一

[`variant.cpp`](variant.cpp)：

```cpp
struct RectRoi   { double x, y, width, height; };
struct CircleRoi { double centerX, centerY, radius; };
struct RingRoi   { double centerX, centerY, innerRadius, outerRadius; };

using Roi = std::variant<RectRoi, CircleRoi, RingRoi>;
```

`Roi` 类型的变量，**任何时刻里面恰好是这三种之一**。它自己记得当前是哪一种。

```cpp
Roi roi = CircleRoi{100, 100, 10};     // 现在是圆
roi = RectRoi{0, 0, 40, 30};           // 现在是矩形
```

### 查询和取值

```cpp
std::holds_alternative<RectRoi>(roi)      // 现在是矩形吗？
std::get<RectRoi>(roi)                    // 取出矩形。如果不是矩形，抛出 std::bad_variant_access
std::get_if<CircleRoi>(&roi)              // 是圆就返回指向它的指针，不是就返回空指针
```

只关心其中一种时，`get_if` 最方便：

```cpp
if (const auto* circle = std::get_if<CircleRoi>(&roi)) return circle->radius > 100;
```

## 3. `std::visit`：每种类型各走各的

要对所有类型分别处理时，用 `std::visit`。它会根据 variant 里实际的类型，调用对应的函数。

### 写法一：函数对象

```cpp
struct AreaVisitor {
    double operator()(const RectRoi& r) const   { return r.width * r.height; }
    double operator()(const CircleRoi& c) const { return kPi * c.radius * c.radius; }
    double operator()(const RingRoi& r) const   { return kPi * (r.outerRadius * r.outerRadius - r.innerRadius * r.innerRadius); }
};

double area(const Roi& roi) { return std::visit(AreaVisitor{}, roi); }
```

### 写法二：一组 lambda

```cpp
template <class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

std::string describe(const Roi& roi) {
    return std::visit(Overloaded{
        [](const RectRoi& r)   { return "矩形 ..."; },
        [](const CircleRoi& c) { return "圆 ..."; },
        [](const RingRoi& r)   { return "圆环 ..."; },
    }, roi);
}
```

`Overloaded` 这两行是一个固定的小工具，作用是把几个 lambda 合成一个函数对象。现在不需要理解它的原理，会用就行。

### 最大的好处：漏掉一种，编译不过

假如以后新增了第四种形状 `PolygonRoi`，把它加进 `variant` 之后，**所有还没处理它的 `std::visit` 都会编译失败**。编译器替你找出了每一处需要修改的地方。用 `int type` 加 `if` 的写法做不到这一点。

## 4. 项目里的 `Value`

[`value.h`](../../../../core/kernel/include/vision/kernel/value.h)：

```cpp
using Value = std::variant<std::monostate, Image, Region, BlobList, double, bool, std::string, Judgement>;
```

端口上流动的数据就是 `Value`。`std::monostate` 表示“空”，放在第一个位置，这样一个没有赋值的 `Value` 默认就是空的。

[`execution_service.cpp`](../../../../core/contexts/execution/application/src/execution_service.cpp) 里的 `describeValue` 用 `std::visit` 把任意一种值变成一段文字，用的是写法一：

```cpp
struct V {
    std::string operator()(std::monostate) const { return "（空）"; }
    std::string operator()(const Image& i) const { return "image " + ...; }
    std::string operator()(const BlobList& b) const { return "blobs " + std::to_string(b.size()); }
    // ...
};
return std::visit(V{}, v);
```

算子读取输入时用的 `ctx.input<Image>("image")`，内部就是 `std::holds_alternative` 加 `std::get`：类型不对时返回一个失败的 `Result`，而不是抛异常（见 [`catalog/api.h`](../../../../core/contexts/catalog/application/include/vision/catalog/api.h)）。

其他用到 variant 的地方：

| 类型 | 定义 |
|---|---|
| `ParamValue` | `std::variant<double, bool, std::string>`：参数值是数值、布尔或文本之一 |
| `Result<T>` | 内部是 `std::variant<T, Error>`：要么是值，要么是错误 |

## 5. 结构化绑定

把一个由几部分组成的东西，一次拆成几个变量。

```cpp
std::tuple<double, double, int> statistics(const std::vector<Roi>& rois);

const auto [total, largest, count] = statistics(rois);     // 拆 tuple
```

最常用的是遍历 `map`：

```cpp
for (const auto& [name, n] : defectCounts) {       // name 是键，n 是值
    std::cout << name << "：" << n << " 处\n";
}
```

对比以前的写法 `it->first`、`it->second`，可读性好很多。项目里遍历 map 都是这样写的。

它也能拆 `pair` 和成员全部公开的结构体。

## 6. 带初始化的 `if`

```cpp
if (const auto it = defectCounts.find("划痕"); it != defectCounts.end()) {
    use(it->second);
}
// it 在这里已经不存在了
```

分号前面是初始化，后面是条件。变量 `it` 只在这个 `if`（包括 `else`）里有效，不会泄漏到外面。项目里处理 `Status` 时大量使用：

```cpp
if (auto st = r.connect(from, to); !st) return st;
```

## 7. 动手运行

**macOS / Linux**：

```bash
clang++ -std=c++17 -Wall -Wextra -g variant.cpp -o variant && ./variant
```

**Windows**：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 variant.cpp && variant.exe
```

## 8. 练习

1. **亲眼看编译器帮你找遗漏**：新增一种形状 `struct PolygonRoi { std::vector<double> xs, ys; };`，把它加进 `Roi` 的 `variant`，然后编译。看报错指向哪里。再把 `AreaVisitor` 和 `describe` 补全（多边形面积用鞋带公式）。
2. **写一个 visit**：写 `Roi moved(const Roi& roi, double dx, double dy)`，返回平移后的 ROI，形状不变。
3. **用 variant 表示状态**：一个节点的执行结果是三种之一：成功（带耗时）、失败（带原因）、跳过（带上游节点的名字）。定义三个结构体和一个 `variant`，写一个函数把它转成一行文字。
4. **读项目代码**：`Value` 的第一个类型为什么是 `std::monostate`？如果去掉它，`Value v;` 默认会是什么？

## 9. 自测题

1. `std::variant<A, B, C>` 和“一个 `int type` 加一堆成员”的结构体相比，好在哪里？
2. `std::get`、`std::get_if`、`std::holds_alternative` 各做什么？
3. 新增一种类型后，用 `std::visit` 的代码和用 `if (type == ...)` 的代码，表现有什么不同？
4. `for (const auto& [key, value] : someMap)` 里的 `key` 和 `value` 各是什么？
5. `if (auto x = f(); x.ok())` 里，`x` 在哪里有效？

## 下一课

[第 8 课：接口与多态——虚函数、依赖倒置](../lesson08/README.md)
