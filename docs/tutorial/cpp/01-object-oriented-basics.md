# 第 1 课：面向对象基础——自己写一个字符串类

> 预计用时：2 小时
> 示例代码：[`examples/lesson01/`](examples/lesson01/)

## 这一课要回答的问题

你已经学过 `class` 的语法：成员变量、成员函数、`public` 和 `private`、构造函数。但语法书很少讲清楚一件事：**为什么要这样做？直接用结构体加几个函数不行吗？**

这一课我们从零写一个字符串类 `MyString`，每一步都先看“不这样做会出什么问题”，再看类是怎么解决它的。选字符串做例子，是因为它**自己管理一块内存**，面向对象里最重要的几件事（封装、构造、析构、拷贝、移动）在它身上全都会遇到。

> 实际项目里请直接用 `std::string`。我们自己写一遍，是为了理解 `std::string`、`std::vector` 以及本项目里的类为什么长成那个样子。

最后会用一个**复数类**做对照：它不管理任何资源，所以上面那些函数一个都不用写。学完你应该能判断：自己要写的类属于哪一种。

## 1. 起点：不用类会怎样

[`examples/lesson01/c_style.cpp`](examples/lesson01/c_style.cpp)：

```cpp
struct Str {
    char* data;        // 指向一块堆内存
    std::size_t size;  // 字符个数（不含结尾的 '\0'）
};

Str strCreate(const char* text);   // 申请内存，复制内容
void strDestroy(Str& s);           // 释放内存
```

这个字符串有一条**必须始终成立的规则**：`data` 指向一块长度为 `size + 1` 的内存，最后一个字符是 `'\0'`。只要这条规则成立，所有操作都是安全的。

问题在于，这种写法**没有任何东西保护这条规则**：

| # | 写法 | 后果 |
|---|---|---|
| 1 | `name.size = 100;` | 任何人都能直接改成员。`size` 说有 100 个字符，实际只有 4 个，之后的读写全部越界 |
| 2 | `Str other;` | 忘记调用 `strCreate`。`data` 是一个随机地址 |
| 3 | `Str copy = name;` | 只复制了指针，两个变量指向同一块内存。各自销毁一次，同一块内存就被释放两次 |
| 4 | 忘记调用 `strDestroy` | 内存泄漏。函数中间有 `return` 或出错提前退出时特别容易漏 |

这四个问题有一个共同点：**规则存在于程序员的脑子里，而不在代码里。** 写的人记得，用的人不一定记得；今天记得，三个月后不一定记得。

面向对象要做的事情，就是**把规则写进代码，让编译器和类本身来保证它**。这种“必须始终成立的规则”有个专门的名字：**不变量**。这个词在本项目的设计文档里会反复出现。

接下来分五步解决这四个问题。

## 2. 第一步：封装——只有自己能改自己的数据

解决问题 1。

```cpp
class MyString {
public:
    std::size_t size() const { return size_; }
    const char* c_str() const { return data_ ? data_ : ""; }
    void append(const MyString& other);
    // ...
private:
    std::size_t size_;
    char* data_;
};
```

成员变量放进 `private`，外面的代码就**写不出** `name.size_ = 100` 了，编译不过。要修改字符串，只能调用 `append` 这样的公开成员函数，而每个成员函数在结束时都会保证不变量仍然成立。

于是需要检查的范围，从“整个程序里所有碰过这个结构体的代码”，缩小到“这个类自己的十几个成员函数”。**这就是封装的意义：不是为了隐藏，而是为了让规则只需要在一个地方被保证。**

`MyString` 的不变量写在了头文件的注释里：

```cpp
// 不变量（任何时刻都成立）：
//   要么是空字符串：data_ == nullptr 且 size_ == 0
//   要么 data_ 指向一块自己独占的、长度为 size_ + 1 的内存，并且 data_[size_] == '\0'
```

> **命名约定**：本项目里，成员变量的名字以下划线结尾（`size_`、`data_`），一眼就能和局部变量、参数区分开。

## 3. 第二步：构造函数——对象一出生就是合法的

解决问题 2。

```cpp
MyString() noexcept;                  // 空字符串
explicit MyString(const char* text);  // 从 C 字符串创建
```

构造函数在对象创建时**自动**运行。有了它，“创建了但还没初始化”的状态就不存在了：

```cpp
MyString a;           // 合法的空字符串
MyString b("blob");   // 合法的、内容为 blob 的字符串
```

实现里用的是**成员初始化列表**（冒号后面那部分），它在进入函数体之前就把成员初始化好：

```cpp
MyString::MyString(const char* text) : size_(std::strlen(text)), data_(nullptr) {
    if (size_ == 0) return;
    data_ = new char[size_ + 1];
    std::memcpy(data_, text, size_ + 1);
}
```

**`explicit` 是什么**：没有它的话，编译器会在需要 `MyString` 的地方，悄悄把一个 `const char*` 转换成 `MyString`（顺便申请一次内存）。加上 `explicit` 就必须明确写出 `MyString("blob")`。**只有一个参数的构造函数，默认都加 `explicit`。**

## 4. 第三步：析构函数——对象消失时自动归还资源

解决问题 4。

```cpp
MyString::~MyString() { delete[] data_; }
```

析构函数在对象生命周期结束时**自动**运行：离开作用域、函数返回、出错提前退出，都会运行。

```cpp
void process() {
    MyString name("blob");
    if (somethingWrong()) return;   // name 在这里自动析构
    doWork(name);
}                                   // name 在这里自动析构
```

你不需要、也不应该手动调用它。**“忘记释放”这件事从此不可能发生。**

这种“在构造函数里获取资源，在析构函数里释放资源”的做法叫 **RAII**，是 C++ 最重要的一个习惯用法。资源不只是内存：文件、锁、网络连接都用同样的方式管理。第 4 课会专门讲它。

## 5. 第四步：拷贝——复制出一份独立的内容

解决问题 3。这一步最容易被忽略，后果也最严重。

### 先看不写拷贝会怎样

如果你只写了构造函数和析构函数，编译器会**自动生成**一个拷贝构造函数，做法是逐个成员复制。对 `MyString` 来说，就是把 `size_` 和 `data_` 这个**指针**复制过去：

```
复制之前：  a.data_ ──► [b][l][o][b][\0]

复制之后：  a.data_ ──► [b][l][o][b][\0] ◄── b.data_      ← 两个对象指向同一块内存
```

`a` 和 `b` 析构时，各自 `delete[]` 一次，同一块内存被释放两次。

[`examples/lesson01/broken_copy.cpp`](examples/lesson01/broken_copy.cpp) 演示了这个问题。带上内存检查工具编译运行：

```bash
clang++ -std=c++17 -g -fsanitize=address broken_copy.cpp -o broken_copy && ./broken_copy
```

会看到：

```
ERROR: AddressSanitizer: attempting double-free on 0x...
```

`-fsanitize=address` 是编译器自带的内存检查工具，能发现重复释放、越界、释放后使用等错误。不开它的话，这个程序可能看起来“正常运行”，然后在某个毫不相干的地方崩溃。第 12 课会详细讲这个工具。

### 正确的拷贝

拷贝的含义应该是：**得到一份内容相同、但完全独立的对象**，改一个不影响另一个。

```cpp
// 拷贝构造：用一个已有的对象创建新对象
MyString::MyString(const MyString& other) : size_(other.size_), data_(nullptr) {
    if (size_ == 0) return;
    data_ = new char[size_ + 1];                  // 申请自己的内存
    std::memcpy(data_, other.data_, size_ + 1);   // 把内容复制过来
}

// 拷贝赋值：把一个已有的对象赋值给另一个已有的对象
MyString& MyString::operator=(const MyString& other) {
    MyString copy(other);   // 先复制出一个临时对象
    swap(copy);             // 再和它交换内容
    return *this;           // copy 离开作用域时，带着“我原来的内存”一起析构
}
```

两者的区别：

```cpp
MyString b = a;   // 拷贝构造：b 是新创建的
c = a;            // 拷贝赋值：c 原来就存在，要先处理掉它原来的内容
```

拷贝赋值用的“先复制、再交换”写法有两个好处：`c = c`（自己赋值给自己）不会出错；如果申请内存失败，`c` 原来的内容保持不变。

## 6. 第五步：移动——把内容搬过去，不复制

拷贝是正确的，但有时候是浪费的：

```cpp
MyString makeGreeting() {
    MyString s("hello");
    s.append(MyString(", blob"));
    return s;              // s 马上就要消失了
}

MyString g = makeGreeting();
```

`s` 在函数返回后就没用了。如果这时还去“申请新内存、复制内容、再释放旧内存”，三步全是白做。更合理的做法是：**直接把 `s` 的那块内存交给 `g`**。这就是移动。

```cpp
// 移动构造：接管对方的内存，然后让对方变成空字符串
MyString::MyString(MyString&& other) noexcept : size_(other.size_), data_(other.data_) {
    other.size_ = 0;
    other.data_ = nullptr;
}
```

```
移动之前：  other.data_ ──► [b][l][o][b][\0]

移动之后：  other.data_ = nullptr（空字符串）
            this->data_ ──► [b][l][o][b][\0]          ← 同一块内存，没有复制
```

几个要点：

- **`MyString&&`** 叫右值引用，可以粗略理解为“一个马上就不再需要的对象”。函数返回的临时对象会自动匹配它。
- **`std::move(x)`** 的意思是“我以后不再需要 `x` 的内容了，可以把它搬走”。它本身不搬任何东西，只是做一个标记，让编译器选择移动而不是拷贝。
- **被移走的对象仍然必须是合法的。** 这里它变成了一个空字符串，可以安全地析构，也可以重新赋值。但**不要再读它的内容**。
- **`noexcept`** 表示这个函数保证不会抛出异常。`std::vector` 扩容时，只有元素的移动构造函数标了 `noexcept`，它才会用移动，否则退回到拷贝。

> **这不是纸上谈兵。** 本项目开发时就出过一个真实的 bug：一个对象被 `std::move` 走之后，代码又去用它里面的字符串，结果拿到的是空字符串，导致所有算子注册到了同一个位置上，只剩下一个。第 3 课会带你看这个 bug 的现场。

## 7. 让它更好用：`const` 和运算符重载

### `const` 成员函数

```cpp
std::size_t size() const { return size_; }
char at(std::size_t index) const;
```

函数后面的 `const` 是一个承诺：**这个函数不会修改对象**。编译器会检查这个承诺。它的作用是：当你通过 `const MyString&` 拿到一个字符串时，只能调用带 `const` 的函数，于是可以确定它不会被改动。

规则很简单：**不修改对象的成员函数，一律加 `const`。**

### 运算符重载

```cpp
friend bool operator==(const MyString& a, const MyString& b);
friend MyString operator+(const MyString& a, const MyString& b);
friend std::ostream& operator<<(std::ostream& out, const MyString& s);
```

有了它们，自己写的类型用起来就和内置类型一样自然：

```cpp
if (a == b) { ... }
MyString full = first + last;
std::cout << full;
```

重载运算符有一条原则：**只在含义显而易见时才重载。** `==` 表示相等、`+` 表示拼接，没有人会误解。如果要想一下才知道 `a * b` 是什么意思，就应该写成一个有名字的函数。

`friend` 的意思是：这个函数不是成员函数，但允许它访问私有成员。

## 8. 五个特殊函数，以及什么时候一个都不用写

到这里，`MyString` 一共写了五个特殊的成员函数：

| 函数 | 什么时候被调用 |
|---|---|
| 析构函数 `~MyString()` | 对象生命周期结束 |
| 拷贝构造 `MyString(const MyString&)` | `MyString b = a;` |
| 拷贝赋值 `operator=(const MyString&)` | `c = a;` |
| 移动构造 `MyString(MyString&&)` | `MyString d = std::move(b);`、函数返回局部对象 |
| 移动赋值 `operator=(MyString&&)` | `c = std::move(a);`、`c = makeGreeting();` |

关于它们有两条经验规则：

- **五法则**：如果一个类需要自己写其中任何一个，那么这五个通常都要写（或者明确禁用）。因为需要自己写，说明这个类在直接管理某种资源。
- **零法则**：如果一个类的成员都能自己管好自己（比如 `std::string`、`std::vector`、智能指针），那么这五个**一个都不用写**，编译器自动生成的就是对的。

### 对照：复数类

[`examples/lesson01/complex_rule_of_zero.cpp`](examples/lesson01/complex_rule_of_zero.cpp)：

```cpp
class Complex {
public:
    constexpr Complex(double re = 0, double im = 0) : re_(re), im_(im) {}

    constexpr double re() const { return re_; }
    constexpr double im() const { return im_; }
    double abs() const { return std::hypot(re_, im_); }

    friend constexpr Complex operator+(Complex a, Complex b) { return {a.re_ + b.re_, a.im_ + b.im_}; }
    friend constexpr Complex operator*(Complex a, Complex b) { ... }
    friend constexpr bool operator==(Complex a, Complex b) { return a.re_ == b.re_ && a.im_ == b.im_; }

private:
    double re_;
    double im_;
};
```

复数只有两个 `double`，不管理任何资源。**没有析构函数，没有拷贝，没有移动**，编译器逐个成员复制就完全正确。它和 `MyString` 的对比：

| | `MyString` | `Complex` |
|---|---|---|
| 管理资源吗 | 是，一块堆内存 | 否 |
| 五个特殊函数 | 全部自己写（五法则） | 一个都不写（零法则） |
| 传参方式 | `const MyString&`（避免复制内存） | 直接传值（只有 16 个字节，复制比引用还快） |
| 按什么判断相等 | 内容 | 两个分量 |

### 最重要的结论

**在实际项目里，你几乎永远不应该自己写这五个函数。** 把资源交给已经写好的类去管理：

| 需要 | 用 |
|---|---|
| 字符串 | `std::string` |
| 动态数组 | `std::vector` |
| 独占一个堆上的对象 | `std::unique_ptr` |
| 多处共享一个对象 | `std::shared_ptr` |

然后你自己的类就都是零法则的类。本项目的核心代码里**没有一个手写的析构函数用来释放内存**，也没有一处 `delete`。

那为什么还要学这一课？因为只有明白了 `MyString` 内部在做什么，你才能看懂：为什么传参要用 `const std::string&`；`std::move` 之后的对象为什么不能再用；为什么 `std::vector` 扩容时旧元素的指针会失效。

## 9. 项目里的例子

打开这几个文件，对照这一课学的内容：

| 文件 | 看什么 |
|---|---|
| [`core/kernel/include/vision/kernel/geometry.h`](../../../core/kernel/include/vision/kernel/geometry.h) 里的 `Roi` | 构造函数是 `private` 的，只能通过 `Roi::rect(...)` 创建，创建时检查宽高大于 0。所有成员函数都是 `const`。**没有**析构、拷贝、移动：零法则 |
| [`core/kernel/include/vision/kernel/image.h`](../../../core/kernel/include/vision/kernel/image.h) 里的 `Image` | 像素存放在 `std::shared_ptr<const std::vector<std::uint8_t>>` 里。复制 `Image` 时只复制这个智能指针，不复制像素。同样没有手写的析构和拷贝：零法则 |
| [`core/contexts/authoring/domain/include/vision/authoring/domain/recipe.h`](../../../core/contexts/authoring/domain/include/vision/authoring/domain/recipe.h) 里的 `Recipe` | 成员变量全部 `private`；修改只能通过 `addNode`、`connect` 等函数；头文件开头的注释列出了它的 6 条不变量。这就是第 2 节的封装，只是规则更多 |

## 10. 面向对象的另外两部分

教科书上常说面向对象有三个特征：封装、继承、多态。这一课只讲了**封装**（以及配套的构造、析构、拷贝、移动），因为它是另外两个的基础，也是工程里用得最多的。

- **多态**（通过基类指针调用派生类的函数）在本项目里用于“接口”：算子、仓储、推理后端都是这样做的。第 8 课专门讲。
- **继承**在现代 C++ 工程里用得很克制，本项目里基本只用于“实现接口”。需要复用代码时，优先让一个类**包含**另一个类的对象，而不是继承它。

## 11. 动手运行

```bash
cd docs/tutorial/cpp/examples/lesson01
FLAGS="-std=c++17 -Wall -Wextra -g -fsanitize=address"

clang++ $FLAGS test.cpp my_string.cpp -o my_string_test && ./my_string_test    # 全部通过
clang++ $FLAGS complex_rule_of_zero.cpp -o complex && ./complex               # 1-2i 全部通过
clang++ $FLAGS broken_copy.cpp -o broken_copy && ./broken_copy                # 报告重复释放
```

Linux 上把 `clang++` 换成 `g++`。

## 12. 练习

1. **运行并观察**：按第 11 节运行三个程序。阅读 `broken_copy` 的报错信息，找到它指出的“第一次释放”和“第二次释放”分别发生在哪一行。
2. **制造一个 bug 再修好**：把 `my_string.cpp` 里的拷贝构造函数改成只复制指针（`data_ = other.data_;`），重新编译运行测试，看内存检查工具报什么错。然后改回去。
3. **加一个功能**：给 `MyString` 增加成员函数 `MyString substr(std::size_t start, std::size_t length) const;`，返回从 `start` 开始、最多 `length` 个字符的子串；`start` 超出范围时抛出 `std::out_of_range`。在 `test.cpp` 里补上测试。写完后想一想：你的实现保持了不变量吗？
4. **写一个零法则的类**：写一个二维向量类 `Vec2`（两个 `double`），支持 `+`、`-`、数乘、`==`、求长度 `length()`。要求：不写析构、拷贝、移动；所有不修改对象的函数都标 `const`。
5. **读项目代码**：打开 `Roi` 的定义。它为什么不提供 `setWidth()` 这样的修改函数？如果要把一个 ROI 向右移动 10 个像素，按它的设计应该怎么做？

## 13. 自测题

1. 什么是不变量？`MyString` 的不变量是什么？
2. 把成员变量设为 `private`，目的是“隐藏数据”吗？如果不是，目的是什么？
3. 一个类只写了构造函数和析构函数，在析构函数里 `delete[]` 了一个指针成员。复制这个类的对象会发生什么？
4. `MyString b = a;` 和 `b = a;` 分别调用哪个函数？
5. `std::move(x)` 做了什么？执行之后还能用 `x` 吗？
6. 五法则和零法则分别是什么？你自己写类的时候，应该尽量让它属于哪一种？怎么做到？
7. `Complex` 作为函数参数时直接传值，`MyString` 却用 `const MyString&`，为什么？

## 下一课

[第 2 课：项目是怎么组织的](README.md)：头文件和源文件为什么要分开，命名空间怎么用，CMake 是怎么把几十个文件编译成一个程序的。
