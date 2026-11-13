# 第 3 课：值、引用、`const` 和移动语义

> 预计用时：2 小时
> 示例代码：就在这个文件夹里，和这份正文放在一起

## 这一课要回答的问题

第 1 课里，你亲手给 `MyString` 写了拷贝和移动，知道了它们在内部做什么。这一课从**使用者**的角度看同一件事：

1. 我写下的这行代码，会不会发生拷贝？
2. 函数的参数应该怎么写：传值、传引用，还是传 `const` 引用？
3. `std::move` 什么时候用，用了之后要注意什么？

最后会带你看本项目里一个真实的 bug。它只有一行，编译器没有任何警告，却让所有算子都注册失败了。

## 1. 为什么要关心拷贝

竞赛里很少关心拷贝，因为数据通常是全局数组和几个 `int`。工程里处理的是这样的东西：

| 对象 | 拷贝一次的代价 |
|---|---|
| `int`、`double`、`Complex` | 几个字节，可以忽略 |
| `std::string`（一个方案的名字） | 申请一次内存，复制几十个字节 |
| `std::vector<Blob>`（一张图上的 500 个连通块） | 申请内存，复制 2 万多个字节 |
| 一张 5MP 的彩色图像（如果像素直接存在对象里） | 复制 1500 万个字节，大约几毫秒 |

我们的检测流程处理一张 5MP 图像总共只用 8 毫秒左右。如果图像在节点之间每传递一次就复制一遍，光是复制就会超过这个时间。

所以工程 C++ 的一个基本功是：**知道自己写的每一行代码有没有在复制东西。**

## 2. 三种传参方式

```cpp
std::size_t lengthByValue(Tracked t);              // 传值
std::size_t lengthByConstRef(const Tracked& t);    // 传 const 引用
void renameByRef(Tracked& t);                      // 传引用
```

| 写法 | 发生了什么 | 函数能修改调用者的对象吗 |
|---|---|---|
| `T t`（传值） | 把调用者的对象**复制**一份给函数 | 不能，改的是副本 |
| `const T& t`（传 const 引用） | **不复制**。`t` 是调用者那个对象的另一个名字，但只能读 | 不能，编译器会阻止 |
| `T& t`（传引用） | **不复制**。`t` 是调用者那个对象的另一个名字，可以改 | 能 |

**引用**就是“另一个名字”。它不是一个新对象，对它做的任何操作都直接作用在原来的对象上。

### 亲眼看一次

[`copy_counter.cpp`](copy_counter.cpp) 里有一个会“记账”的类型 `Tracked`：每次被拷贝或移动，就把计数加一。运行结果：

```
传值：拷贝 1 次，移动 0 次
传 const 引用：拷贝 0 次，移动 0 次
传引用：拷贝 0 次，移动 0 次
传值 + std::move：拷贝 0 次，移动 1 次
for (auto item : list)，3 个元素：拷贝 3 次，移动 0 次
for (const auto& item : list)：拷贝 0 次，移动 0 次
add(a)：拷贝 1 次，移动 1 次
add(std::move(a))：拷贝 0 次，移动 2 次
add(Tracked("temp"))：拷贝 0 次，移动 1 次
```

第 5、6 行值得特别注意：**循环变量写成 `auto` 时，每个元素都会被复制一份。** 这是从竞赛转到工程时最常见的无意拷贝。

```cpp
for (auto item : list) { ... }          // 每次循环复制一个元素
for (const auto& item : list) { ... }   // 不复制
```

### 该用哪一种

| 情况 | 写法 | 例子 |
|---|---|---|
| 只读，类型很小（`int`、`double`、`bool`、枚举、指针，以及像 `Complex` 这样只有几个数的类型） | 传值 `T` | `double area(double width, double height)` |
| 只读，其他类型 | `const T&` | `int countBlobs(const BinaryGrid& grid)` |
| 需要修改调用者的对象 | `T&` | `void markBlob(..., std::vector<bool>& visited)` |
| 函数要把参数**保存下来**（存进成员变量或容器） | 传值 `T`，然后 `std::move` 进去 | 见第 5 节 |

拿不准的时候，默认用 `const T&`。

## 3. `const`：一个由编译器检查的承诺

`const` 在三个地方出现，含义都是“不会修改”：

```cpp
void print(const MyString& s);          // 这个函数不会修改 s
std::size_t size() const;               // 这个成员函数不会修改对象
const double ratio = overlapRatio(a, b);// 这个变量初始化之后不再改变
```

它的价值是让读代码的人**不用读实现**就能确定一些事情。看到 `countBlobs(const BinaryGrid& grid)`，就知道调用它之后 `grid` 一定没变。

两条习惯：

- **局部变量算出来之后不再改的，声明成 `const`。** 读代码的人就不用担心它在后面某处被改掉。本项目的代码里到处是这种写法。
- **不修改对象的成员函数，一律加 `const`。** 否则通过 `const T&` 拿到对象的人没法调用它。

## 4. 返回值，以及悬空引用

### 返回值：直接按值返回

```cpp
MyString makeGreeting() {
    MyString s("hello");
    return s;              // 不会复制：编译器要么直接在调用者的位置构造它，要么移动它
}
```

**函数返回一个局部对象时，直接按值返回。** 不要为了“省一次拷贝”而返回引用或指针，也不要写 `return std::move(s);`，那样反而会妨碍编译器优化。

### 绝对不要返回局部变量的引用

```cpp
const std::string& bad() {
    std::string s = "hello";
    return s;              // s 在函数结束时就销毁了，返回的引用指向一个已经不存在的对象
}
```

引用只是“另一个名字”。原来的对象没了，这个名字就指向一块无效的内存，叫做**悬空引用**。

### 更隐蔽的一种：容器扩容

[`dangling.cpp`](dangling.cpp)：

```cpp
std::vector<int> areas;
areas.push_back(42);

const int& first = areas[0];                        // first 是 areas[0] 的另一个名字

for (int i = 0; i < 100; ++i) areas.push_back(i);   // vector 空间不够，换了一块更大的内存

std::cout << first << "\n";                         // first 还指着已经释放的旧内存
```

`std::vector` 空间不够时，会申请一块更大的内存，把元素搬过去，再释放旧内存。之前拿到的引用、指针、迭代器全部失效。

这个程序**不开内存检查时不会崩溃**，只是输出一个莫名其妙的数字（我运行时是 1989899378，本该是 42），退出码还是 0。这正是这类错误危险的地方：它不报错，只是悄悄给你错误的结果。

打开内存检查再运行：

```bash
clang++ -std=c++17 -g -fsanitize=address dangling.cpp -o dangling && ./dangling
```

```
ERROR: AddressSanitizer: heap-use-after-free on address 0x...
    #0 ... in main .../dangling.cpp:17
```

它直接指出了出错的行号。

**规则：往容器里添加或删除元素之后，之前拿到的引用就不要再用了。** 需要的话重新取一次，或者保存下标而不是引用。

## 5. 移动：`std::move` 怎么用

### 什么时候用

第 1 课讲过，移动是“把内容搬过去，不复制”。`std::move(x)` 的意思是：**“我以后不再需要 `x` 的内容了。”**

最常见的用法是“函数要把参数保存下来”：

```cpp
struct Holder {
    std::vector<Tracked> items;
    void add(Tracked t) { items.push_back(std::move(t)); }   // 按值接收，再 move 进容器
};
```

这样写，由调用者决定要不要付出拷贝的代价：

```cpp
holder.add(a);              // 我还要继续用 a → 拷贝 1 次
holder.add(std::move(a));   // 我不再需要 a 了 → 0 次拷贝
holder.add(Tracked("x"));   // 临时对象 → 0 次拷贝
```

构造函数里也是同样的写法。第 0 课的 `BinaryGrid`，以及本项目的 `Recipe`、`RecipeVersion` 的构造函数都是这样：

```cpp
RecipeVersion::RecipeVersion(RecipeVersionId id, std::string recipeName, std::vector<Node> nodes, ...)
    : id_(std::move(id)),
      recipeName_(std::move(recipeName)),
      nodes_(std::move(nodes)), ...
```

### 用了之后要注意什么

**一个对象被 `std::move` 之后，不要再读它的内容。** 标准只保证它处于“某个合法的状态”，可以析构，可以重新赋值，但里面是什么没有保证。实际上通常是空的。

```cpp
std::string a = "blob";
std::string b = std::move(a);
std::cout << a.size();      // 不要这样做。结果通常是 0，但不该依赖它
a = "again";                // 重新赋值之后可以继续用
```

### `std::move` 本身什么都不做

这一点很重要：`std::move(x)` 只是给 `x` 贴上一个“可以搬走”的标记，**真正的搬运发生在它被用来构造或赋值给另一个对象的时候**。所以：

- 对 `const` 对象用 `std::move` 没有效果，仍然会拷贝（`const` 对象不允许被修改，自然也不能被搬空）。
- 对 `int`、`double` 这样的类型用 `std::move` 没有意义，它们的移动就是拷贝。

## 6. 一个真实的 bug

现在看本项目开发 POC-1 时真实出现的 bug。[`registry_bug.cpp`](registry_bug.cpp) 是它的最小重现。

### 现象

平台启动时注册了 5 个算子，但查询时只能找到 1 个。集成测试全部失败，报“找不到算子 `vision.to_gray`”。**编译没有任何警告。**

### 出问题的代码

```cpp
void add(OperatorMeta meta) {
    const auto& id = meta.id;                       // ① id 是 meta.id 的另一个名字
    entries_.emplace(id, Entry{std::move(meta)});   // ② 问题在这一行
}
```

先自己想一想，哪里错了？

### 原因

关键在于第 ② 行里几件事发生的**先后顺序**：

1. 先构造第二个参数 `Entry{std::move(meta)}`。`meta` 的内容被搬进了 `Entry`，于是 **`meta.id` 变成了空字符串**。
2. 然后 `emplace` 才用第一个参数 `id` 去构造键。但 `id` 是引用，它就是 `meta.id`，**这时已经是空的了**。

结果：每个算子都以空字符串 `""` 为键。第一个放进去了，后面的因为键重复，全部被丢弃。

这个 bug 同时用到了这一课的两个知识点：

- **引用是另一个名字**，不是一份副本。原对象变了，它也跟着变。
- **被 `std::move` 之后的对象不能再读。** 这里并没有直接写 `meta.id`，而是通过引用 `id` 间接读到了它，所以更难发现。

### 修复

```cpp
void add(OperatorMeta meta) {
    const std::string id = meta.id;                 // 复制一份。id 现在是独立的字符串
    entries_.emplace(id, Entry{std::move(meta)});
}
```

只改了一个地方：`const auto&` 改成 `const std::string`。多复制了一个短字符串，换来的是正确。

运行示例：

```
有 bug 的版本：注册了 3 个算子，目录里有 1 个；能找到 vision.threshold 吗？不能
修复后的版本：注册了 3 个算子，目录里有 3 个；能找到 vision.threshold 吗？能
```

项目里修复后的代码在 [`core/contexts/catalog/application/src/operator_catalog.cpp`](../../../../core/contexts/catalog/application/src/operator_catalog.cpp)，旁边留了一行注释说明原因。同时补了一个回归测试 [`tests/catalog_test.cpp`](../../../../tests/catalog_test.cpp)，保证以后不会再犯。

### 从这个 bug 学到什么

1. **`std::move(x)` 所在的那个语句里，不要再以任何方式用到 `x`**，包括通过引用、指针间接用到。
2. **引用适合用在“短时间内借来看一眼”的场合。** 如果被引用的对象接下来要被移走或修改，就复制一份。
3. **测试能发现编译器发现不了的错误。** 这个 bug 是被集成测试抓到的，从出现到修复只用了几分钟。

## 7. 动手运行

在 `docs/tutorial/cpp/lesson03` 目录下。

**macOS / Linux**：

```bash
FLAGS="-std=c++17 -Wall -Wextra -g -fsanitize=address"

clang++ $FLAGS copy_counter.cpp -o copy_counter && ./copy_counter
clang++ $FLAGS registry_bug.cpp -o registry_bug && ./registry_bug
clang++ $FLAGS dangling.cpp -o dangling && ./dangling          # 故意有错：报告 heap-use-after-free
```

**Windows**（在“x64 Native Tools Command Prompt for VS 2022”里）：

```bat
set FLAGS=/std:c++17 /W4 /EHsc /utf-8 /Zi /fsanitize=address

cl %FLAGS% copy_counter.cpp && copy_counter.exe
cl %FLAGS% registry_bug.cpp && registry_bug.exe
cl %FLAGS% dangling.cpp && dangling.exe
```

## 8. 项目里的例子

| 文件 | 看什么 |
|---|---|
| [`core/kernel/include/vision/kernel/image.h`](../../../../core/kernel/include/vision/kernel/image.h) | `Image` 的像素放在 `std::shared_ptr` 里，所以复制一个 `Image` 只复制一个指针，不复制像素。这就是图像可以在节点之间按值传递的原因 |
| [`core/contexts/authoring/domain/include/vision/authoring/domain/recipe.h`](../../../../core/contexts/authoring/domain/include/vision/authoring/domain/recipe.h) | 看每个成员函数的参数：`const NodeId&`、`const PortRef&` 是只读的；`RoiSpec roi`、`std::string name` 按值接收，是因为要保存下来 |
| [`core/contexts/authoring/domain/src/recipe.cpp`](../../../../core/contexts/authoring/domain/src/recipe.cpp) | 搜索 `std::move`，看它们各自把什么搬进了哪里；搜索 `const auto&`，看循环变量的写法 |
| [`core/contexts/execution/domain/src/executor.cpp`](../../../../core/contexts/execution/domain/src/executor.cpp) | `outputs[i] = std::move(res).value();` 把算子的输出搬进结果表，没有复制 |

## 9. 练习

1. **运行并对照**：运行 `copy_counter`，把输出和第 2 节的表格逐行对照，确认每一行你都能解释。
2. **预测再验证**：在 `copy_counter.cpp` 的 `main` 里加上下面的代码，**先预测**拷贝和移动各几次，再运行验证。

   ```cpp
   Tracked::reset();
   std::vector<Tracked> v;          // 注意：这次没有 reserve
   for (int i = 0; i < 5; ++i) v.push_back(Tracked("x"));
   std::cout << Tracked::copies << " " << Tracked::moves << "\n";
   ```

   移动的次数为什么比 5 多？和 `dangling.cpp` 演示的是不是同一件事？
3. **修一个悬空引用**：修改 `dangling.cpp`，让它正确输出 42。至少想出两种改法。
4. **找 bug**：下面的函数想把两个名字拼起来存进列表。它有什么问题？怎么改？

   ```cpp
   void addFullName(std::vector<std::string>& names, std::string first, std::string last) {
       names.push_back(std::move(first));
       names.push_back(first + " " + last);
   }
   ```
5. **读项目代码**：打开 `recipe.cpp` 里的 `Recipe::addNode`。函数里先创建了一个局部变量 `node`，最后 `nodes_.push_back(node);` 再 `return node.id;`。如果把 `push_back(node)` 改成 `push_back(std::move(node))` 来省一次拷贝，后面那行 `return node.id;` 会出什么问题？应该怎么写才既省拷贝又正确？

## 10. 自测题

1. `void f(T t)`、`void f(const T& t)`、`void f(T& t)` 三种写法分别在什么情况下用？
2. `for (auto x : items)` 和 `for (const auto& x : items)` 有什么区别？
3. 什么是悬空引用？举出两种会产生它的情况。
4. `std::move(x)` 本身做了什么？真正的“搬运”发生在什么时候？
5. 对象被 `std::move` 之后，哪些操作是安全的，哪些不是？
6. 第 6 节的 bug 里，代码并没有在 `std::move(meta)` 之后直接写 `meta.id`，为什么还是出错了？
7. 一个函数要把传进来的字符串保存到成员变量里，参数应该怎么写？为什么？

## 下一课

[第 4 课：资源管理——RAII 与智能指针](../lesson04/README.md)
