# 第 9 课：lambda、`std::function` 与 STL 算法

> 预计用时：1.5 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

竞赛里你已经用过 `sort(a, a + n, cmp)`。这一课把它系统化：

1. lambda 是什么，方括号里的“捕获”是怎么回事？
2. 有哪些现成的算法可以代替手写循环？为什么要代替？
3. 怎么把“一段行为”当作参数传给函数？

## 1. 为什么不直接写循环

判断“有没有面积超过 100 的连通块”：

```cpp
bool found = false;
for (std::size_t i = 0; i < blobs.size(); ++i) {
    if (blobs[i].area > 100) { found = true; break; }
}
```

```cpp
const bool found = std::any_of(blobs.begin(), blobs.end(), [](const Blob& b) { return b.area > 100; });
```

第二种写法：

- **意图写在名字里。** `any_of` 就是“有没有任何一个”，不用读完循环体才知道它在干什么。
- **没有下标，没有 `break`，没有中间变量。** 也就没有越界、漏 `break`、忘记初始化这些错误。
- **结果可以是 `const`。**

循环本身没有错。当循环做的事正好是“查找、计数、判断、排序、变换、删除”之一时，用对应的算法更清楚。

## 2. lambda：就地写一个小函数

```cpp
[minArea](const Blob& b) { return b.area >= minArea; }
 ───┬───  ──────┬──────   ───────────┬──────────────
   捕获        参数                  函数体
```

lambda 是一个没有名字的函数对象，可以存进变量、传给函数、从函数返回。

### 捕获：lambda 怎么用到外面的变量

lambda 的函数体里**不能直接使用**外面的局部变量，必须在方括号里写明要用哪些、怎么用：

| 写法 | 含义 |
|---|---|
| `[]` | 什么都不捕获 |
| `[x]` | 按值捕获 `x`：lambda 里保存一份**副本** |
| `[&x]` | 按引用捕获 `x`：lambda 里用的就是外面那个 `x` |
| `[this]` | 捕获当前对象，可以访问成员 |
| `[=]` / `[&]` | 把用到的全部按值 / 按引用捕获。**不推荐**，看不出到底捕获了什么 |

按值和按引用的区别（[`algorithms.cpp`](algorithms.cpp)）：

```cpp
double threshold = 100;
const auto byValue = [threshold](const Blob& b) { return b.area >= threshold; };
const auto byRef   = [&threshold](const Blob& b) { return b.area >= threshold; };
threshold = 10;      // 之后修改

std::count_if(..., byValue)   // 2：仍然用创建时的 100
std::count_if(..., byRef)     // 4：用的是修改后的 10
```

### 捕获的陷阱：悬空引用又来了

```cpp
std::function<bool(const Blob&)> largerThan(double minArea) {
    return [&minArea](const Blob& b) { return b.area >= minArea; };   // 错
}
```

`minArea` 是函数的参数，函数返回后它就不存在了，而 lambda 里还留着对它的引用。这和第 3 课的悬空引用是同一个问题。

规则：

- **lambda 只在当前函数里马上用掉**（比如传给 `std::sort`）：按引用捕获没有问题。
- **lambda 会被保存下来、之后才调用**（存进成员变量、从函数返回、交给另一个线程）：**按值捕获**。

第 16 课的多线程代码里，这条规则非常关键。

## 3. 常用算法

都在 `<algorithm>` 和 `<numeric>` 里。前两个参数是要处理的范围，一般写 `v.begin(), v.end()`。

### 查询

| 算法 | 作用 |
|---|---|
| `std::any_of(b, e, 条件)` | 有没有至少一个满足 |
| `std::all_of(b, e, 条件)` | 是不是全部满足 |
| `std::none_of(b, e, 条件)` | 是不是一个都不满足 |
| `std::count_if(b, e, 条件)` | 有几个满足 |
| `std::find_if(b, e, 条件)` | 找第一个满足的，返回迭代器；找不到返回 `e` |
| `std::max_element(b, e, 比较)` | 最大的那个，返回迭代器 |

`find_if` 的结果要先和 `end()` 比较，才能使用：

```cpp
const auto it = std::find_if(blobs.begin(), blobs.end(), [](const Blob& b) { return b.id == 3; });
if (it != blobs.end()) use(*it);
```

### 排序

```cpp
std::sort(v.begin(), v.end(), [](const Blob& a, const Blob& b) { return a.area > b.area; });   // 面积从大到小
```

比较函数的含义是“`a` 是否应该排在 `b` 前面”。注意必须用 `>` 或 `<`，不能用 `>=` 或 `<=`，否则行为没有保证，可能崩溃。

### 累加、变换

```cpp
const double total = std::accumulate(v.begin(), v.end(), 0.0,
                                     [](double sum, const Blob& b) { return sum + b.area; });

std::vector<int> ids;
std::transform(v.begin(), v.end(), std::back_inserter(ids), [](const Blob& b) { return b.id; });
```

`accumulate` 的初始值写 `0.0` 而不是 `0`：写成 `0` 的话，累加会按 `int` 进行，小数部分被丢掉。

`std::back_inserter(ids)` 表示“把结果依次追加到 `ids` 末尾”。

### 删除：erase-remove 惯用法

从 `vector` 里删掉所有满足条件的元素：

```cpp
v.erase(std::remove_if(v.begin(), v.end(), [](const Blob& b) { return b.area < 20; }), v.end());
```

这是一个固定写法。`remove_if` 并不真的删除，它只是把要保留的元素挪到前面，返回“新的末尾”；`erase` 再把后面多余的部分真正删掉。两步缺一不可。

**不要在循环里边遍历边 `erase`**，那样很容易跳过元素或者用到失效的迭代器。

## 4. `std::function`：把行为当参数

```cpp
std::vector<Blob> filter(const std::vector<Blob>& blobs, const std::function<bool(const Blob&)>& keep);

filter(blobs, largerThan(100));
filter(blobs, [](const Blob& b) { return b.centerX > 40; });    // 换一个条件，filter 不用改
```

`std::function<bool(const Blob&)>` 的意思是“任何接收一个 `const Blob&`、返回 `bool` 的可调用对象”。lambda、普通函数、函数对象都能装进去。

它适合用在需要**保存**一段行为的地方：回调、事件处理器、工厂函数。只是临时传给一个函数用一下时，用模板参数更高效（标准库的算法都是这样做的），不过在不追求极致性能的地方，`std::function` 更简单。

## 5. 项目里的例子

| 位置 | 用法 |
|---|---|
| [`recipe.cpp`](../../../../core/contexts/authoring/domain/src/recipe.cpp) `Recipe::connect` | `std::any_of` 判断输入端口是否已经有连线（不变量 R3） |
| `recipe.cpp` `Recipe::removeNode` | erase-remove 惯用法：删除与某个节点相连的所有连线（不变量 R6） |
| `recipe.cpp` `Recipe::findNode` | `std::find_if` 按 ID 查找节点 |
| [`inspection.cpp`](../../../../core/contexts/execution/domain/src/inspection.cpp) `Inspection::complete` | `std::any_of` 和 `std::none_of` 检查不变量 I2、I4 |
| [`catalog/api.h`](../../../../core/contexts/catalog/application/include/vision/catalog/api.h) | `using OperatorFactory = std::function<std::unique_ptr<Operator>()>;`：保存“怎么创建一个算子” |
| [`in_memory_event_bus.h`](../../../../adapters/eventbus/include/vision/eventbus/in_memory_event_bus.h) | `using Handler = std::function<void(const EventPtr&)>;`：保存事件处理器 |
| [`apps/cli/main.cpp`](../../../../apps/cli/main.cpp) | `bus.subscribe("*", [this](const EventPtr& e) { ... });`：捕获 `this` 的 lambda 作为订阅者 |

打开 `Recipe::removeNode`，把它和第 3 节的 erase-remove 写法对照一下。

## 6. 动手运行

**macOS / Linux**：

```bash
clang++ -std=c++17 -Wall -Wextra -g -fsanitize=address algorithms.cpp -o algorithms && ./algorithms
```

**Windows**：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 algorithms.cpp && algorithms.exe
```

## 7. 练习

1. **改写循环**：用算法改写下面三段代码。

   ```cpp
   // (a)
   int n = 0;
   for (const auto& b : blobs) if (b.centerY > 50) ++n;
   // (b)
   double smallest = 1e18;
   for (const auto& b : blobs) if (b.area < smallest) smallest = b.area;
   // (c)
   std::vector<double> areas;
   for (const auto& b : blobs) areas.push_back(b.area);
   ```
2. **多级排序**：把 `blobs` 排序，规则是先按 `centerY` 从小到大，`centerY` 相同时按 `centerX` 从小到大（也就是从上到下、从左到右的阅读顺序）。
3. **找出陷阱**：下面的代码有什么问题？用内存检查工具验证你的判断，然后修好。

   ```cpp
   std::function<double()> makeCounter() {
       double count = 0;
       return [&count]() { return ++count; };
   }
   ```
4. **读项目代码**：在 `recipe.cpp` 里找到 `Recipe::publish()`。它用哪个算法检查“至少有一个节点提供判定依据”（规则 P5）？如果手写循环来做，需要几行？

## 8. 自测题

1. lambda 的三个部分分别是什么？
2. `[x]` 和 `[&x]` 有什么区别？各自什么时候用？
3. 什么情况下按引用捕获会出问题？
4. `find_if` 找不到时返回什么？使用结果前要做什么？
5. 从 `vector` 里删除所有满足条件的元素，标准写法是什么？为什么需要两步？
6. `std::accumulate(v.begin(), v.end(), 0, ...)` 累加一组 `double` 有什么问题？
7. `std::function<void(int)>` 能装下哪些东西？

## 下一课

[第 10 课：模板入门与宏](../lesson10/README.md)
