# 第 10 课：模板入门与宏

> 预计用时：1.5 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

到现在你已经用了很多带尖括号的东西：`std::vector<int>`、`Result<PortRef>`、`ctx.input<Image>("image")`。还见过一行神秘的 `VISION_REGISTER_OPERATOR(ThresholdOp)`。

1. 尖括号里的类型是怎么回事？怎么自己写一个？
2. 模板为什么要写在头文件里？
3. 算子“一行注册”是怎么做到的？宏是什么，为什么平时不提倡用？

这一课的目标是**能看懂、会用**，以及写简单的模板。模板的高级用法很深，工程里绝大多数时候用不到。

## 1. 函数模板

把一个值限制在范围内，`int` 要一份，`double` 要一份，逻辑一模一样：

```cpp
int clampTo(int value, int low, int high) { ... }
double clampTo(double value, double low, double high) { ... }
```

函数模板让你只写一次（[`templates.cpp`](templates.cpp)）：

```cpp
template <typename T>
T clampTo(T value, T low, T high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}
```

`T` 是一个“类型的占位符”。调用时，编译器根据参数的类型**自动生成**对应的版本：

```cpp
clampTo(300, 0, 255);            // T = int
clampTo(-0.5, 0.0, 1.0);         // T = double
clampTo<double>(3, 0.5, 2.5);    // 参数类型不一致时，明确指定 T
```

模板不是一个函数，而是一份**生成函数的配方**。每用到一种新类型，编译器就按配方生成一个真正的函数。

对 `T` 的要求来自函数体里用到的操作。这里用了 `<` 和 `>`，所以任何支持这两个运算符的类型都能用。传一个不支持的类型进来，会在编译时报错。模板的报错信息通常很长，**从第一条错误开始读**，后面的大多是连带的。

## 2. 类模板

项目里的 `Result<T>` 就是类模板。示例里是一个简化版：

```cpp
template <typename T>
class MiniResult {
public:
    static MiniResult success(T value) { ... }
    static MiniResult failure(std::string message) { ... }

    bool ok() const { return value_.has_value(); }
    const T& value() const { return *value_; }
    const std::string& message() const { return message_; }
private:
    std::optional<T> value_;
    std::string message_;
};
```

```cpp
MiniResult<int> parseDigit(char c);
MiniResult<std::string> ...
```

`MiniResult<int>` 和 `MiniResult<std::string>` 是两个**完全不同的类**，只是由同一份配方生成。

## 3. 成员函数模板

类本身不是模板，但某个成员函数是：

```cpp
class Settings {
public:
    template <typename T>
    std::optional<T> get(const std::string& key) const;
};

settings.get<int>("threshold");      // 调用者指定想要的类型
settings.get<bool>("invert");
```

这正是算子里读取输入的写法：

```cpp
auto in = ctx.input<Image>("image");        // 我想要一个 Image
auto blobs = ctx.input<BlobList>("blobs");  // 我想要一个 BlobList
```

打开 [`catalog/api.h`](../../../../core/contexts/catalog/application/include/vision/catalog/api.h) 看 `OpContext::input`：

```cpp
template <typename T>
Result<T> input(const std::string& name) const {
    auto it = inputs_.find(name);
    if (it == inputs_.end() || !std::holds_alternative<T>(it->second)) {
        return Result<T>::failure("operator.input", "输入 " + name + " 缺失或类型不对");
    }
    return std::get<T>(it->second);
}
```

第 6、7 课的内容在这里汇合了：端口上的值是 `variant`，`input<T>` 检查它是不是 `T` 类型，是就取出来，不是就返回失败的 `Result<T>`。

## 4. 模板为什么写在头文件里

第 2 课说过，函数的定义应该放在源文件里。模板是例外：**模板的定义必须放在头文件里。**

原因是编译器要在**用到模板的地方**按配方生成代码，这时它必须看得到完整的配方。如果配方藏在另一个 `.cpp` 里，编译当前文件时看不到，就生成不出来，链接时会报“未定义的引用”。

所以 `result.h` 里 `Result<T>` 的全部实现都在头文件里。

## 5. 宏

### 宏是什么

```cpp
#define SQUARE(x) x * x
```

宏在**预处理阶段**做纯文本替换。编译器真正开始编译时，宏已经不存在了，它看到的是替换后的文本。

```cpp
SQUARE(1 + 2)      // 被替换成 1 + 2 * 1 + 2，结果是 5，不是 9
```

### 为什么平时不提倡用

| 问题 | 说明 |
|---|---|
| 只是文本替换，不懂 C++ | 上面的优先级错误；参数被计算多次 |
| 不受命名空间约束 | 一个宏会影响它之后的所有代码。本项目遇到过：Windows 的系统头文件把 `ERROR` 定义成了宏，导致我们的枚举值 `ERROR` 编译失败，只好改名为 `Error` |
| 调试困难 | 调试器里看不到宏，报错信息指向展开后的代码 |

所以现代 C++ 里：

| 以前用宏 | 现在用 |
|---|---|
| `#define N 1005` | `constexpr int kN = 1005;` |
| `#define SQUARE(x) ...` | 函数模板或 `inline` 函数 |
| `#define FOR(i, n) ...` | 直接写 `for` |

宏的名字约定用全大写，就是为了提醒读代码的人：这是宏，小心。

### 宏仍然有用的地方

有几件事只有宏能做，其中一件正是本项目需要的：**根据一个名字生成一段重复的代码**。

## 6. 算子的一行注册是怎么实现的

新增算子时，文件最后有一行：

```cpp
VISION_REGISTER_OPERATOR(ThresholdOp)
```

没有任何地方“调用”过注册函数，`main` 里也没有算子列表，但程序启动时算子已经在目录里了。示例里有一个最小重现，原理分三步。

### 第一步：全局变量在 `main` 之前初始化

```cpp
const bool registered = Registry::global().add("text.upper", ...);
```

这是一个全局变量。全局变量的初始值在 `main` 开始之前就要算出来，于是 `Registry::global().add(...)` 在 `main` 之前被调用了。**我们并不关心这个 `bool` 的值，只是借用“初始化”这个时机来执行一段代码。**

### 第二步：用宏省掉重复的部分

每个算子都要写上面那一行，只有类名不同。用宏把它包起来：

```cpp
#define REGISTER_OPERATOR(OpClass, id)                                                  \
    namespace {                                                                         \
    [[maybe_unused]] const bool registered_##OpClass =                                  \
        Registry::global().add(id, [] { return std::unique_ptr<Operator>(new OpClass()); }); \
    }
```

- `\`：宏定义换行。
- `##`：把两段文字拼成一个标识符。`registered_##OpClass` 在 `OpClass` 是 `UpperOp` 时变成 `registered_UpperOp`，这样每个算子的变量名都不一样。
- 匿名命名空间：这个变量只在当前文件可见，不会和别的文件冲突。
- `[[maybe_unused]]`：告诉编译器“这个变量确实不会被用到，不要警告”。
- 传给 `add` 的 lambda 是一个工厂：需要时调用它，就能创建一个这种算子。

### 第三步：保证目录先于算子存在

```cpp
static Registry& global() {
    static Registry instance;     // 函数内的静态变量：第一次调用时才创建
    return instance;
}
```

不同文件里的全局变量，谁先初始化是**没有保证**的。如果 `Registry` 本身也是一个普通的全局变量，就可能出现“算子去注册时，目录还没创建”的情况。把它放在函数里，就保证了第一次有人调用 `global()` 时它一定已经创建好。

### 一个实际遇到的坑

这种注册方式依赖“全局变量会被初始化”。如果把算子编译成一个普通的静态库，链接器发现程序里没有任何代码引用这些算子文件，就会把它们**整个丢掉**，算子自然也就没有注册。

本项目的解决办法写在 [`operators/CMakeLists.txt`](../../../../operators/CMakeLists.txt) 里：算子用 CMake 的 `OBJECT` 库，目标文件直接参与链接，不会被丢弃。

项目里的宏在 [`catalog/api.h`](../../../../core/contexts/catalog/application/include/vision/catalog/api.h) 的最后，和示例几乎一样，区别是它调用算子类的 `meta()` 取得完整的元数据。

## 7. 动手运行

**macOS / Linux**：

```bash
clang++ -std=c++17 -Wall -Wextra -g -fsanitize=address templates.cpp -o templates && ./templates
```

**Windows**：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 templates.cpp && templates.exe
```

## 8. 练习

1. **写一个函数模板**：`template <typename T> T maxOf(const std::vector<T>& items)`，返回最大的元素（假设不为空）。分别用 `int`、`double`、`std::string` 的 `vector` 测试。然后传一个 `std::vector<Blob>`（`Blob` 没有 `<` 运算符）进去，读一读编译器的报错，找到真正说明原因的那一行。
2. **写一个类模板**：`template <typename T> class Stack`，内部用 `std::vector<T>`，提供 `push`、`pop`（返回 `std::optional<T>`，空时返回空）、`size`。
3. **注册第三个算子**：在示例里新增 `TrimOp`（去掉字符串首尾的空格），用宏注册为 `"text.trim"`。`main` 里除了增加检查之外，需要改别的地方吗？
4. **看宏展开后的样子**：用编译器只做预处理，看 `REGISTER_OPERATOR(UpperOp, "text.upper")` 被替换成了什么。macOS / Linux：`clang++ -std=c++17 -E templates.cpp | tail -80`；Windows：`cl /std:c++17 /utf-8 /EP templates.cpp`。

## 9. 自测题

1. 函数模板和普通函数的区别是什么？“模板是配方”是什么意思？
2. `MiniResult<int>` 和 `MiniResult<std::string>` 是同一个类吗？
3. `ctx.input<Image>("image")` 里的 `<Image>` 起什么作用？
4. 模板的定义为什么必须写在头文件里？
5. 宏和函数的本质区别是什么？举一个宏出错的例子。
6. 算子的一行注册依赖哪个语言特性，让代码在 `main` 之前就被执行？
7. `Registry::global()` 为什么把实例放在函数内部的静态变量里，而不是直接做成全局变量？

## 下一课

[第 11 课：单元测试——GoogleTest](../lesson11/README.md)
