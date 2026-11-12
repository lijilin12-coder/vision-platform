# 第 6 课：错误处理——`Result`、`optional`、异常

> 预计用时：1.5 小时
> 示例代码：[`examples/lesson06/`](examples/lesson06/)

## 这一课要回答的问题

竞赛里输入总是合法的，程序不需要处理错误。工程里正好相反：用户会输错参数，文件会不存在，图像会损坏，相机会断开。

1. 函数执行不下去时，怎么告诉调用者？
2. C++ 有好几种报告错误的方式，各自什么时候用？
3. 本项目里到处出现的 `Result` 和 `Status` 是什么？

## 1. 先把“错误”分成三类

不同的错误应该用不同的方式处理。先学会分类：

| 类别 | 是什么 | 例子 | 谁的责任 |
|---|---|---|---|
| **① 没有结果是正常的** | 不算错误，只是“没有” | 在方案里查找一个节点，没找到 | 没有人做错 |
| **② 预料之中的失败** | 业务上本来就可能发生，调用者要知道原因并做出反应 | 用户连了一条会形成环的线；图像文件损坏；参数超出范围 | 外部输入 |
| **③ 程序写错了** | 代码本身有 bug，正确的程序里不该发生 | 下标越界；对空指针取值；违反了函数的使用前提 | 写代码的人 |

对应的工具：

| 类别 | 用什么 |
|---|---|
| ① 没有结果是正常的 | `std::optional<T>` |
| ② 预料之中的失败 | `Result<T>`（本项目自己定义的类型） |
| ③ 程序写错了；以及构造函数里的失败 | 异常 |

## 2. `std::optional`：可能没有

```cpp
std::optional<double> findParam(const std::map<std::string, double>& params, const std::string& name) {
    const auto it = params.find(name);
    if (it == params.end()) return std::nullopt;   // 没有
    return it->second;                              // 有
}
```

`std::optional<double>` 的意思是“可能有一个 `double`，也可能没有”。使用时必须先判断：

```cpp
if (const auto v = findParam(params, "value")) {    // 有值时条件为真
    std::cout << *v;                                 // 用 * 取出里面的值
}

double value = findParam(params, "missing").value_or(0.0);   // 没有时用默认值
```

**为什么不用特殊值表示“没有”？** 竞赛里常用 `-1` 或 `INF` 表示“没找到”。问题是 `-1` 本身可能是合法的结果，而且调用者很容易忘记检查，把 `-1` 当成正常值继续算下去。`optional` 把“没有”变成了类型的一部分，忘记处理时代码写起来就不顺。

## 3. `Result`：成功带着值，失败带着原因

`optional` 只能说“没有”，说不出**为什么**没有。用户连线被拒绝时，界面要告诉他是因为“类型不兼容”还是“会形成环”。这就需要 `Result`。

`Result<T>` 是本项目在 [`result.h`](../../../core/common/include/vision/common/result.h) 里定义的类型，里面要么是一个 `T`（成功），要么是一个 `Error`（失败）：

```cpp
struct Error {
    std::string code;      // 给程序看：例如 "authoring.cycle"，界面可以据此决定怎么提示
    std::string message;   // 给人看：例如 "连线 n3.image → n1.image 会形成环"
};
```

### 返回 `Result`

```cpp
Result<PortRef> parsePortRef(const std::string& text) {
    const auto dot = text.find('.');
    if (dot == std::string::npos) {
        return Result<PortRef>::failure("port_ref.no_dot", "端口引用必须形如 n1.image：" + text);
    }
    if (dot == 0 || dot + 1 == text.size()) {
        return Result<PortRef>::failure("port_ref.empty_part", "节点名和端口名都不能为空：" + text);
    }
    return PortRef{text.substr(0, dot), text.substr(dot + 1)};    // 成功：直接返回值
}
```

### 使用 `Result`

```cpp
const auto r = parsePortRef("n1.image");
if (r.ok()) {
    use(r.value());                          // 成功：取值
} else {
    std::cerr << r.error().message;          // 失败：取原因
}
```

### 没有返回值的操作：`Status`

`Status` 就是 `Result<void>`，只表示成功或失败：

```cpp
Status checkThreshold(double value) {
    if (value < 0 || value > 255) return Status::failure("param.out_of_range", "阈值必须在 0～255 之间");
    return Status::success();
}
```

### 把错误往上传

一个函数调用了几个可能失败的步骤，任何一步失败都原样返回：

```cpp
Result<std::string> describeEdge(const std::string& from, const std::string& to) {
    auto f = parsePortRef(from);
    if (!f) return f.error();        // 第一步失败：把错误原样交给上一层
    auto t = parsePortRef(to);
    if (!t) return t.error();
    return f.value().node + " → " + t.value().node;
}
```

项目里到处是这三行一组的写法。它比异常啰嗦，但好处是**每一处可能失败的地方都看得见**。

### `[[nodiscard]]`：不许无视

`Result` 的定义前面有 `[[nodiscard]]`。意思是：如果调用了一个返回 `Result` 的函数却没有使用返回值，编译器会给出警告。这样就不会出现“连线其实失败了，但代码当它成功了”这种事。

## 4. 异常：不该发生的事

```cpp
double Matrix::at(int r, int c) const {
    if (r < 0 || r >= rows_ || c < 0 || c >= cols_) throw std::out_of_range("Matrix::at 下标越界");
    return data_[...];
}
```

```cpp
try {
    const Matrix m(2, 2);
    m.at(5, 5);
} catch (const std::out_of_range& e) {     // 按 const 引用捕获
    std::cerr << e.what();
}
```

抛出异常后，程序会一层层退出当前的函数，直到遇到匹配的 `catch`。途中所有局部对象都会被析构，这就是第 4 课的 RAII 如此重要的原因。

异常适合两种情况：

- **程序写错了**（第 ③ 类）。越界访问是调用者的 bug。这种错误通常不该在附近处理，而是让它一路传到最外层，记录下来，然后修代码。
- **构造函数里的失败。** 构造函数没有返回值，只能靠异常。但如果失败是预料之中的，更好的做法是第 5 课的工厂函数。

### 为什么不全部用异常

很多语言里所有错误都用异常。本项目选择把预料之中的失败做成返回值，原因是：

| | 异常 | `Result` |
|---|---|---|
| 从函数签名能看出它会失败吗 | 看不出 | 看得出，返回类型就写着 |
| 调用者会不会忘记处理 | 会，忘了就一路传上去，可能让程序终止 | 不容易，不处理就取不到值，编译器还会警告 |
| 失败时的开销 | 大 | 和正常返回一样 |
| 适合 | 很少发生、附近处理不了的错误 | 经常发生、调用者要立刻做出反应的错误 |

“用户连了一条不合法的线”每天会发生很多次，界面必须马上给出提示。这是业务流程的一部分，不是意外。

还有一条设计上的理由：**被拒绝的命令不是领域事件，只是一个返回值**（见 [`05-tactical-design.md`](../../ddd/05-tactical-design.md) §0）。`Recipe::connect` 返回 `Status`，正好表达了这一点。

### 项目里的约定

| 位置 | 做法 |
|---|---|
| 领域层的命令（`connect`、`setParam`、`publish`……） | 返回 `Status` 或 `Result<T>` |
| 值对象的工厂函数（`Roi::rect`、`Image::create`） | 返回 `Result<T>` |
| 查找（`OperatorCatalog::find`、`RecipeRepository::find`） | 返回 `std::optional<T>` |
| 算子的 `execute` | 返回 `Status`；执行引擎会额外捕获算子抛出的异常，转成“节点执行失败” |
| 对失败的 `Result` 调用 `value()` | 抛出 `std::logic_error`：这是调用者的 bug |

## 5. 断言：检查“我认为一定成立的事”

```cpp
#include <cassert>
assert(index < size);    // 我确信这里 index 一定小于 size
```

断言用来检查**程序内部的假设**。条件不成立时程序立刻终止，并报告是哪一行。它只在调试版本里生效，发布版本里会被整个去掉，所以：

- **不要用断言检查外部输入。** 用户输入、文件内容用 `Result`。
- **断言的条件里不要有副作用。** `assert(queue.pop())` 在发布版本里连 `pop` 都不会执行。

## 6. 项目里的例子

打开 [`recipe.cpp`](../../../core/contexts/authoring/domain/src/recipe.cpp) 里的 `Recipe::connect`。它依次检查：节点是否存在、端口是否存在、类型是否兼容、输入端口是否已被占用、是否会形成环。每一项不满足，就返回一个带错误码的 `Status`：

```cpp
if (!isCompatible(out->type, in->type)) {
    return reject("type_mismatch", std::string("端口类型不兼容：") + toString(out->type) + " → " + toString(in->type));
}
```

再看测试 [`authoring_domain_test.cpp`](../../../tests/authoring_domain_test.cpp)，它正是通过错误码来确认“是因为正确的原因被拒绝的”：

```cpp
EXPECT_EQ(st.error().code, "authoring.cycle");
```

## 7. 动手运行

这个示例用到了项目里的 `result.h`，编译时要用 `-I` 告诉编译器头文件在哪。在 `docs/tutorial/cpp/examples/lesson06` 目录下：

**macOS / Linux**：

```bash
clang++ -std=c++17 -Wall -Wextra -g -I../../../../../core/common/include errors.cpp -o errors && ./errors
```

**Windows**：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 /I..\..\..\..\..\core\common\include errors.cpp && errors.exe
```

## 8. 练习

1. **分类**：下面的情况各属于第 1 节的哪一类，应该用哪种方式报告？
   - 读取图像文件，文件不存在
   - 在算子目录里查找一个算子 ID，没有这个算子
   - 用户设置的 ROI 宽度是负数
   - 代码里对一个空的 `vector` 调用了 `front()`
   - 模型文件的格式和声明的不一致
2. **写一个返回 `Result` 的函数**：`Result<int> parsePositiveInt(const std::string& text)`，把字符串转成正整数。空字符串、含有非数字字符、等于零，分别返回不同错误码的失败。在 `main` 里补上检查。
3. **传递错误**：写 `Result<double> parseRatio(const std::string& a, const std::string& b)`，用上一题的函数解析两个数，返回它们的比值。任何一步失败都把错误原样传上来。
4. **读项目代码**：`Recipe::publish()` 在方案不能发布时，是怎么把**多个**问题一次性告诉调用者的？为什么不是发现第一个问题就返回？

## 9. 自测题

1. 错误可以分成哪三类？各用什么方式处理？
2. `std::optional` 相比“用 -1 表示没找到”，好在哪里？
3. `Result<T>` 和 `std::optional<T>` 的区别是什么？
4. `Error` 里为什么既有 `code` 又有 `message`？
5. 本项目为什么不把“连线被拒绝”做成异常？
6. `[[nodiscard]]` 有什么用？
7. 断言适合检查什么，不适合检查什么？

## 下一课

[第 7 课：`variant`、`visit` 与结构化绑定](07-variant-and-bindings.md)
