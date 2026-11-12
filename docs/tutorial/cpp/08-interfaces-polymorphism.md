# 第 8 课：接口与多态——虚函数、依赖倒置

> 预计用时：1.5 小时
> 示例代码：[`examples/lesson08/`](examples/lesson08/)

## 这一课要回答的问题

1. 工位要从相机取图，测试时却没有相机。怎么让同一份工位代码既能用相机，也能用文件夹，还能用假数据？
2. 虚函数、纯虚函数、`override`、`final` 分别是什么？
3. 设计文档里反复出现的“端口”“适配器”“依赖倒置”，落到代码上是什么样子？

这是面向对象三个特征里的后两个：继承和多态。第 1 课说过，它们在本项目里几乎只用于一件事：**定义接口，再实现接口**。

## 1. 问题：直接依赖具体的类

```cpp
class Station {
    Camera camera_;                       // 工位直接包含一个相机
public:
    void runOnce() { auto frame = camera_.grab(); ... }
};
```

这样写，`Station` 就和相机绑死了：

- **没法测试。** 要测试工位的逻辑，必须真的接上一台相机。
- **没法换。** 想从文件夹读图做离线检测，得改 `Station` 的代码。
- **依赖方向不对。** 工位的业务逻辑（什么时候取图、什么时候热切换）是核心，相机驱动是技术细节。核心依赖了细节，细节一变，核心就得跟着改。

## 2. 接口：只说能做什么

[`examples/lesson08/interfaces.cpp`](examples/lesson08/interfaces.cpp)：

```cpp
class ImageSource {
public:
    virtual ~ImageSource() = default;
    virtual std::optional<Frame> next() = 0;
    virtual std::string describe() const = 0;
};
```

- **`virtual`**：这个函数可以被派生类重新实现。
- **`= 0`**：纯虚函数。这里只有声明，没有实现，派生类**必须**实现它。
- 含有纯虚函数的类不能直接创建对象，它只是一份“约定”：凡是图像源，都能 `next()`，都能 `describe()`。

C++ 没有专门的 `interface` 关键字。**一个只有纯虚函数（外加虚析构函数）的类，就是接口。**

### 虚析构函数为什么必须有

```cpp
std::unique_ptr<ImageSource> source = std::make_unique<FolderSource>(...);
```

`source` 销毁时，是通过基类指针去析构的。基类的析构函数如果不是虚函数，就只会运行基类的析构，派生类的成员（比如 `FolderSource` 里的文件列表）不会被正确释放。

规则：**有虚函数的类，析构函数一律写成 `virtual ~X() = default;`**。

## 3. 实现接口

```cpp
class FolderSource final : public ImageSource {
public:
    explicit FolderSource(std::vector<std::string> files) : files_(std::move(files)) {}

    std::optional<Frame> next() override {
        if (index_ >= files_.size()) return std::nullopt;
        return Frame{640, 480, files_[index_++]};
    }
    std::string describe() const override { ... }
private:
    std::vector<std::string> files_;
    std::size_t index_ = 0;
};
```

- **`: public ImageSource`**：`FolderSource` 是一种 `ImageSource`。
- **`override`**：告诉编译器“我在重写基类的虚函数”。如果函数名拼错了、参数写错了、漏了 `const`，没有 `override` 时编译器会默默把它当成一个新函数，基类的版本没有被重写；有了 `override`，编译器会报错。**重写虚函数时一律加 `override`。**
- **`final`**：这个类不允许再被继承。本项目里的实现类都加了它。

## 4. 多态：使用者只认识接口

```cpp
class Station {
public:
    explicit Station(std::unique_ptr<ImageSource> source) : source_(std::move(source)) {}

    int runAll() {
        int count = 0;
        while (const auto frame = source_->next()) { ... }
        return count;
    }
private:
    std::unique_ptr<ImageSource> source_;
};
```

`Station` 里只出现了 `ImageSource`。`source_->next()` 具体调用哪个类的 `next`，要到**运行时**根据 `source_` 实际指向的对象才决定。这就是**多态**。

```cpp
Station fromFolder(std::make_unique<FolderSource>(files));
Station fromCamera(std::make_unique<CameraSource>(2));
```

同一个 `Station` 类，一行代码都不用改，就能用两种图像源。

把依赖从外面通过构造函数传进来，这个做法叫**依赖注入**。

### 测试时换成假的

```cpp
class FakeSource final : public ImageSource { ... };   // 不需要相机，也不需要文件

Station underTest(std::make_unique<FakeSource>(5));
check(underTest.runAll() == 5, "处理了 5 帧");
```

这是接口最实际的好处：**核心逻辑可以脱离硬件单独测试。**

## 5. 依赖倒置

对比前后两种结构：

```
之前：  Station ──依赖──► Camera                    核心依赖细节

之后：  Station ──依赖──► ImageSource（接口）
                             ▲
                             │ 实现
              FolderSource / CameraSource / FakeSource     细节依赖核心定义的接口
```

关键在于**接口归谁所有**。`ImageSource` 是按照工位的需要定义的，属于核心这一边。相机、文件夹这些技术细节反过来去适应它。依赖的方向倒过来了，所以叫**依赖倒置**。

这就是设计文档里“六边形架构”的全部技术基础：

| 设计文档的说法 | 代码里是什么 |
|---|---|
| 端口 | 核心定义的接口（一个纯虚类） |
| 适配器 | 实现这个接口的类，放在 `adapters/` 下 |
| 跨平台 = 换适配器 | 鸿蒙上用 `MsLiteBackend`，Windows 上用 `OrtBackend`，核心代码不变 |
| 组装根 | 在 `main` 里决定具体创建哪个实现，再传给核心 |

## 6. 对象切片：按值传递基类的陷阱

```cpp
std::string nameByValue(Shape s) { return s.name(); }          // 按值
std::string nameByRef(const Shape& s) { return s.name(); }     // 按引用

const Circle circle;
nameByRef(circle);     // "圆"
nameByValue(circle);   // "形状"  ← 不是“圆”
```

按值传递时，会用 `circle` 复制出一个 `Shape` 对象。复制的只是 `Shape` 那一部分，`Circle` 特有的部分被“切掉”了，得到的是一个货真价实的 `Shape`。

规则：**多态的对象，只通过引用或指针（包括智能指针）传递，不要按值传递。**

## 7. 项目里的接口

| 接口 | 定义在 | 实现 | 作用 |
|---|---|---|---|
| [`Operator`](../../../core/contexts/catalog/application/include/vision/catalog/api.h) | 算子目录 | `operators/src/` 下的每个算子 | 策略模式：执行引擎只调用 `execute()`，不关心是哪个算法 |
| [`RecipeRepository`](../../../core/contexts/authoring/domain/include/vision/authoring/domain/repositories.h) | 方案编排的**领域层** | [`JsonRecipeRepository`](../../../adapters/persistence/include/vision/persistence/json_repositories.h)，在 `adapters/` 下 | 领域层不知道方案存在文件里还是数据库里 |
| [`NodeRunner`](../../../core/contexts/execution/domain/include/vision/execution/domain/plan.h) | 检测执行的**领域层** | 应用层的 `CatalogNodeRunner`；测试里的 `FakeRunner` | 领域层不依赖算子目录，也不依赖 OpenCV |
| [`EventPublisher`](../../../core/common/include/vision/common/domain_event.h) | 通用组件 | [`InMemoryEventBus`](../../../adapters/eventbus/include/vision/eventbus/in_memory_event_bus.h) | 应用层只管发布，不关心谁在订阅 |

看一下 [`execution_domain_test.cpp`](../../../tests/execution_domain_test.cpp) 里的 `FakeRunner`：它按算子的输出类型返回固定的假数据。有了它，执行计划、检测、判定这些核心逻辑的测试完全不需要图像和 OpenCV，11 个测试在几毫秒内跑完。这和示例里的 `FakeSource` 是同一个做法。

## 8. 接口什么时候不该用

接口不是越多越好。每多一个接口，就多一层间接，读代码时要多跳一次。

| 该用接口 | 不必用接口 |
|---|---|
| 确实有多种实现（多个平台、多种存储） | 只有一种实现，也看不出会有第二种 |
| 需要在测试里替换（硬件、网络、文件、时钟） | 纯计算的值对象，直接用就好 |
| 要隔离易变的外部系统 | |

另外，如果“几种之一”的集合是固定的、并且是数据而不是行为，第 7 课的 `variant` 往往比继承更合适。本项目里，ROI 的形状、端口上的值用 `variant`；算子、仓储、推理后端用接口。

## 9. 动手运行

**macOS / Linux**：

```bash
clang++ -std=c++17 -Wall -Wextra -g -fsanitize=address interfaces.cpp -o interfaces && ./interfaces
```

**Windows**：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 interfaces.cpp && interfaces.exe
```

## 10. 练习

1. **看 `override` 怎么救你**：把 `FolderSource::describe` 后面的 `const` 删掉，编译，看报错。再把 `override` 也删掉重新编译：这次能通过吗？运行结果会怎样？（然后改回去。）
2. **加一种实现**：写 `class RepeatSource final : public ImageSource`，把同一帧重复给出 N 次。`Station` 需要改吗？
3. **给接口加能力**：给 `ImageSource` 增加纯虚函数 `virtual void reset() = 0;`，让图像源可以从头再来。编译器会告诉你哪些类需要补上实现。
4. **读项目代码**：`RecipeRepository` 这个接口定义在方案编排的**领域层**，而它的实现 `JsonRecipeRepository` 在 `adapters/` 下。为什么接口不和实现放在一起？如果把接口也放到 `adapters/` 里，依赖方向会变成什么样？

## 11. 自测题

1. 什么样的类算是“接口”？
2. `virtual`、`= 0`、`override`、`final` 各是什么意思？
3. 有虚函数的类为什么必须有虚析构函数？
4. 什么是多态？`source_->next()` 调用哪个函数是什么时候决定的？
5. 用自己的话解释“依赖倒置”：倒过来的是什么？
6. 什么是对象切片？怎么避免？
7. 用假实现做测试有什么好处？

## 下一课

[第 9 课：lambda、`std::function` 与 STL 算法](09-lambdas-algorithms.md)
