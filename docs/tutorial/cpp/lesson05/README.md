# 第 5 课：类的设计——封装、不变量、值对象

> 预计用时：1.5 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

第 1 课讲了类的**机制**（封装、构造、析构、拷贝、移动）。这一课讲**怎么设计**一个类：

1. 怎么让一个类“创建不出非法的对象”？
2. 值对象和实体有什么区别，各自怎么写？
3. 行为应该放在类里，还是放在类外面的函数里？

这一课的内容和项目的领域设计文档直接对应。学完之后再读 `Roi`、`Recipe`，会发现它们就是按这里的方法写的。

## 1. 反面例子

公差由三个数组成：标称值、下偏差、上偏差。比如孔径 5.00，下偏差 −0.05，上偏差 +0.05。

```cpp
struct LooseTolerance {
    double nominal;
    double lower;    // 应该 ≤ 0
    double upper;    // 应该 ≥ 0
};

LooseTolerance t{5.0, 0.05, -0.05};   // 上下偏差写反了，但编译和运行都不会报错
```

用这个公差去判定，合格范围是 5.05 到 4.95，下限比上限还大，**所有零件都会被判为不合格**。而错误的源头（写反了两个数）和出问题的地方（产线上全部 NG）可能相隔很远，很难查。

如果每个使用公差的函数都自己检查一遍，就又回到了第 1 课说的老问题：规则散落各处，迟早有一个地方忘了检查。

## 2. 让非法状态创建不出来

[`measurement.cpp`](measurement.cpp)：

```cpp
class Tolerance {
public:
    static std::optional<Tolerance> create(double nominal, double lower, double upper) {
        if (!std::isfinite(nominal) || !std::isfinite(lower) || !std::isfinite(upper)) return std::nullopt;
        if (lower > 0 || upper < 0) return std::nullopt;      // 下偏差不能为正，上偏差不能为负
        if (lower == 0 && upper == 0) return std::nullopt;    // 公差带不能为零
        return Tolerance(nominal, lower, upper);
    }

    static std::optional<Tolerance> symmetric(double nominal, double plusMinus) {
        return create(nominal, -plusMinus, plusMinus);
    }
    // ...
private:
    Tolerance(double nominal, double lower, double upper) : ... {}
    double nominal_, lower_, upper_;
};
```

做法有三步：

1. **构造函数设为 `private`。** 外面的代码不能直接创建对象。
2. **提供一个 `static` 的工厂函数**，它先检查参数，合法才创建。
3. **不提供任何能把对象改成非法状态的函数。**

结果是：**只要你手里有一个 `Tolerance`，它就一定是合法的。** 所有接收 `Tolerance` 的函数都不需要再检查，检查只发生在创建的那一刻，也就是离错误源头最近的地方。

```cpp
Tolerance::create(5.0, 0.05, -0.05)   // 返回空：写反了，当场就被发现
```

### 为什么用工厂函数，不直接在构造函数里检查

构造函数没有返回值，发现参数不合法时只能抛异常。而“用户输入了一个不合法的公差”是预料之中的情况，用返回值表达更合适（第 6 课会详细比较）。工厂函数可以返回 `std::optional` 或 `Result`，让调用者必须面对“可能失败”这件事。

工厂函数还有一个好处：**可以起名字**。`Tolerance::symmetric(5.0, 0.05)` 比 `Tolerance(5.0, -0.05, 0.05)` 清楚得多。

## 3. 值对象

`Tolerance` 是一个**值对象**：它没有身份，完全由它的属性决定。两个标称值和偏差都相同的公差，就是同一个公差。

值对象有三个特点，都体现在代码里：

### 不可变

没有 `setNominal()` 这样的函数。要“修改”，就返回一个新对象：

```cpp
Tolerance shifted(double delta) const { return Tolerance(nominal_ + delta, lower_, upper_); }

const Tolerance moved = tol.shifted(1.0);   // tol 本身没有变
```

不可变带来的好处：

- **不会出现“我改了这里，那里莫名其妙也变了”**。第 3 课的引用陷阱在不可变对象上不存在。
- **多线程下不需要加锁。** 多个线程同时读同一个对象，永远是安全的。
- **不变量只需要在创建时检查一次。**

### 按值比较

```cpp
friend bool operator==(const Tolerance& a, const Tolerance& b) {
    return a.nominal_ == b.nominal_ && a.lower_ == b.lower_ && a.upper_ == b.upper_;
}
```

### 行为跟着数据走

和公差有关的计算，放在 `Tolerance` 自己身上：

```cpp
double min() const { return nominal_ + lower_; }
double max() const { return nominal_ + upper_; }
bool contains(double value) const { return value >= min() && value <= max(); }
```

对比一下两种写法：

```cpp
if (value >= t.nominal + t.lower && value <= t.nominal + t.upper)   // 散落在各处，每处都可能写错
if (tolerance.contains(value))                                       // 只有一处实现，读起来像一句话
```

`Measurement`（测量值）也是值对象，它自己会判断是否合格、超差多少，还能生成给操作员看的判定依据：

```cpp
const Measurement ng("孔径 B", 5.08, tol);
ng.pass();        // false
ng.deviation();   // 0.03
ng.reason();      // "孔径 B 超差 +0.030000 mm"
```

## 4. 实体

有些东西**有身份**：两个属性完全相同的对象，仍然是两个不同的东西。这叫**实体**。

```cpp
class InspectionRecord {
public:
    explicit InspectionRecord(std::string id) : id_(std::move(id)) {}

    void add(Measurement m) { measurements_.push_back(std::move(m)); }   // 状态可以变

    friend bool operator==(const InspectionRecord& a, const InspectionRecord& b) {
        return a.id_ == b.id_;                                            // 只比较 ID
    }
private:
    std::string id_;
    std::vector<Measurement> measurements_;
};
```

| | 值对象 | 实体 |
|---|---|---|
| 靠什么区分 | 属性的值 | 身份（ID） |
| 能修改吗 | 不能，要改就换一个新的 | 能，改了之后还是它 |
| 怎么比较 | 所有属性相等 | ID 相等 |
| 例子 | 公差、测量值、ROI、位姿、判定 | 检测记录、节点、方案、工位 |

判断方法只有一个问题：**两个属性完全相同的它，需要区分吗？** 同一张图检测两次，结果一模一样，但它们是两次检测，统计时要算两次，所以检测记录是实体。两个 5.00±0.05 的公差没有任何区别，所以公差是值对象。

**能用值对象的地方，就不要用实体。** 值对象简单、安全、好测试。

## 5. 几条设计准则

| 准则 | 说明 |
|---|---|
| **成员变量一律 `private`** | 只有纯粹的数据集合、没有任何规则时，才用成员全部公开的 `struct`（比如第 2 课的 `Rect`） |
| **不要给每个成员都配一对 getter 和 setter** | 那和把成员公开没有区别。只提供业务上有意义的操作 |
| **函数名用业务语言** | `tolerance.contains(value)`、`recipe.connect(from, to)`，而不是 `setEdges(...)` |
| **能标 `const` 的成员函数都标上** | 值对象的所有成员函数都应该是 `const` |
| **单参数构造函数加 `explicit`** | 防止悄悄的类型转换 |
| **一个类只做一件事** | 如果很难用一句话说清它是什么，就该拆开 |

## 6. 项目里的例子

| 类 | 对照 |
|---|---|
| [`Roi`](../../../../core/kernel/include/vision/kernel/geometry.h) | 值对象。私有构造函数加工厂函数 `Roi::rect(...)`，宽高不是正数就创建失败；没有任何 setter；有 `operator==`。和 `Tolerance` 的写法一模一样 |
| [`Image`](../../../../core/kernel/include/vision/kernel/image.h) | 值对象。`Image::create` 检查像素数量和宽高是否一致 |
| [`Node`](../../../../core/contexts/authoring/domain/include/vision/authoring/domain/model.h) | 实体，有 `id`。它是成员全部公开的 `struct`，因为它被 `Recipe` 完全包在里面，外面只能拿到只读的引用 |
| [`Recipe`](../../../../core/contexts/authoring/domain/include/vision/authoring/domain/recipe.h) | 实体，也是“聚合根”。规则更多（不能有环、端口类型要匹配……），但思路相同：成员私有，只提供 `addNode`、`connect`、`publish` 这些有业务含义的操作，每个操作都保证规则不被破坏 |

设计文档 [`05-tactical-design.md`](../../../ddd/05-tactical-design.md) 里的“值对象”“实体”“不变量”“聚合”，说的就是这一课的内容。

## 7. 动手运行

**macOS / Linux**：

```bash
clang++ -std=c++17 -Wall -Wextra -g -fsanitize=address measurement.cpp -o measurement && ./measurement
```

**Windows**：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 measurement.cpp && measurement.exe
```

## 8. 练习

1. **写一个值对象**：写 `class Angle`，内部用度数保存，范围规定为 [0, 360)。提供工厂函数 `Angle::degrees(double)`（超出范围时自动折算进范围，比如 370 变成 10，−10 变成 350）、`double degrees() const`、`double radians() const`、`Angle rotated(double deltaDegrees) const`，以及 `operator==`。想一想：这个类的不变量是什么？
2. **扩展**：给 `Tolerance` 增加 `Tolerance widened(double factor) const`，把公差带放大若干倍（标称值不变）。`factor` 不是正数时怎么办？返回类型应该是什么？
3. **判断**：下面这些是值对象还是实体？说出理由。
   - 一个缺陷（类别、位置、面积）
   - 一个工位
   - 一次复判
   - 一个颜色（R、G、B）
   - 一个上线包
4. **读项目代码**：打开 `Recipe` 的头文件。它有 `nodes()` 这个只读访问函数，但没有 `setNodes()`。如果有人需要“把节点 n3 的参数改掉”，他应该调用哪个函数？这个函数会检查什么？

## 9. 自测题

1. “让非法状态创建不出来”具体是怎么做到的？分哪几步？
2. 为什么用工厂函数而不是直接在构造函数里检查参数？
3. 值对象有哪三个特点？
4. 值对象不可变有什么好处？
5. 怎么判断一个概念该做成值对象还是实体？
6. 给每个成员变量都配上 getter 和 setter，算不算封装？为什么？

## 下一课

[第 6 课：错误处理——`Result`、`optional`、异常](../lesson06/README.md)
