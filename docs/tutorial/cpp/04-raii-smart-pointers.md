# 第 4 课：资源管理——RAII 与智能指针

> 预计用时：1.5 小时
> 示例代码：[`examples/lesson04/`](examples/lesson04/)

## 这一课要回答的问题

第 1 课里，`MyString` 在构造函数里申请内存，在析构函数里释放内存。这一课把这个做法推广开：

1. 除了内存，还有哪些东西需要“用完归还”？
2. 怎么保证不管函数怎么退出，归还都一定发生？
3. 不想自己写析构函数时，用什么现成的工具？

## 1. 问题：成对的操作很容易漏掉一半

程序里有很多必须成对出现的操作：

| 获取 | 归还 |
|---|---|
| 申请内存 | 释放内存 |
| 打开相机 | 关闭相机 |
| 打开文件 | 关闭文件 |
| 加锁 | 解锁 |
| 加载模型 | 释放模型 |

手动管理时，只要中间有一条提前退出的路径，就会漏掉归还（[`examples/lesson04/raii.cpp`](examples/lesson04/raii.cpp)）：

```cpp
bool grabManual(bool failInTheMiddle) {
    const int handle = openCamera("手动管理");
    if (failInTheMiddle) return false;   // ← 忘了 closeCamera：相机一直开着
    closeCamera(handle);
    return true;
}
```

函数越长、退出的路径越多，就越容易漏。更麻烦的是**异常**：函数中间任何一个调用抛出异常，后面的 `closeCamera` 都不会执行，而你从代码上根本看不出哪一行可能抛异常。

在产线上，这类错误的后果很直接：相机被占着打不开，或者内存一点点涨上去，运行几天后程序崩溃。

## 2. RAII：把归还放进析构函数

```cpp
class Camera {
public:
    explicit Camera(const std::string& name) : handle_(openCamera(name)) {}
    ~Camera() { closeCamera(handle_); }

    Camera(const Camera&) = delete;              // 禁止拷贝
    Camera& operator=(const Camera&) = delete;

    int grab() const { return handle_; }

private:
    int handle_;
};
```

```cpp
bool grabRaii(bool failInTheMiddle) {
    Camera camera("RAII");
    if (failInTheMiddle) return false;   // camera 在这里自动析构 → 相机被关闭
    (void)camera.grab();
    return true;
}
```

C++ 保证：**局部对象离开作用域时，析构函数一定会被调用**。正常返回、提前返回、抛出异常，都一样。所以只要把归还写进析构函数，就不可能漏掉。

这个做法叫 **RAII**（Resource Acquisition Is Initialization，“资源获取即初始化”）。名字很拗口，意思很简单：**用一个对象的生命周期来管理一份资源**，对象在，资源就在；对象没了，资源就还回去了。

### `= delete`：明确禁止拷贝

`Camera` 的拷贝被禁止了。原因和第 1 课的 `BrokenString` 一样：如果允许复制，两个对象拿着同一个句柄，析构时相机会被关闭两次。有些资源本来就不该被复制，这时就用 `= delete` 明确禁止。别人写了 `Camera b = a;` 会直接编译失败。

### RAII 不只管资源

任何“离开作用域时必须做的事”都可以用它，比如计时：

```cpp
{
    ScopedTimer timer("求和");
    // ……要计时的代码……
}   // timer 在这里析构，打印耗时
```

第 15 课会看到最常用的一个：`std::lock_guard`，构造时加锁，析构时解锁。

## 3. 智能指针：现成的 RAII 工具

需要在堆上创建对象时，不要写 `new` 和 `delete`，用标准库的智能指针。

### `std::unique_ptr`：独占

一个对象只有一个主人。主人没了，对象就被释放。

```cpp
std::unique_ptr<Model> loadModel(const std::string& name) {
    return std::make_unique<Model>(name);       // 用 make_unique 创建，不用 new
}

std::unique_ptr<Model> model = loadModel("scratch-detector");
// auto copy = model;                           // 编译错误：unique_ptr 不能复制
install(std::move(model), installed);           // 可以移动：所有权转移过去
// 这之后 model 是空指针
```

`unique_ptr` **不能复制，只能移动**。所以“谁是主人”在任何时刻都是明确的，并且写在了代码里：

| 函数签名 | 告诉读代码的人 |
|---|---|
| `std::unique_ptr<Model> loadModel()` | 我创建了一个模型，交给你负责 |
| `void install(std::unique_ptr<Model> model)` | 把模型交给我，以后归我管 |
| `void run(const Model& model)` | 我只是借用一下，不负责它的生死 |

`unique_ptr` 没有额外开销，和裸指针一样快。**需要堆上的对象时，默认用它。**

### `std::shared_ptr`：共享

一个对象可以有多个主人。内部有一个计数器记着有几个主人，最后一个主人没了，对象才被释放。

```cpp
std::shared_ptr<const Pixels> a = std::make_shared<Pixels>(5000000);
std::shared_ptr<const Pixels> b = a;    // 只复制指针，计数变成 2；500 万个字节没有复制
```

**只在确实需要共享时才用它。** 它比 `unique_ptr` 慢一点（计数器要保证多线程安全），更重要的是“谁负责这个对象”变得不清楚了。

### 用哪个

| 情况 | 用 |
|---|---|
| 对象可以直接放在栈上，或作为成员变量 | 什么指针都不用 |
| 需要在堆上，只有一个主人 | `std::unique_ptr` |
| 确实有多个主人，谁也不知道自己是不是最后一个 | `std::shared_ptr` |
| 只是借用，不负责生死 | 引用 `T&`，或者裸指针 `T*`（可能为空时） |

**裸指针没有被禁止**，它的含义变成了“借用，不拥有”。被禁止的是用裸指针表示所有权，也就是手写 `new` 和 `delete`。

## 4. 项目里的例子

| 位置 | 用了什么 | 为什么 |
|---|---|---|
| [`image.h`](../../../core/kernel/include/vision/kernel/image.h)：`std::shared_ptr<const std::vector<std::uint8_t>> pixels_` | `shared_ptr` | 一张图像会同时被多个节点持有：灰度节点的输出既是阈值节点的输入，调试时还要显示。谁也不知道自己是不是最后一个用它的。指向 `const`，保证共享的像素不会被任何人修改 |
| [`catalog/api.h`](../../../core/contexts/catalog/application/include/vision/catalog/api.h)：`Result<std::unique_ptr<Operator>> create(...)` | `unique_ptr` | 工厂函数创建一个算子实例，交给调用者独占 |
| [`execution/api.h`](../../../core/contexts/execution/application/include/vision/execution/api.h)：`std::unique_ptr<Impl> impl_` | `unique_ptr` | 把实现细节藏在源文件里，头文件里只留一个指针。这样检测执行的领域类不会出现在公开头文件里，其他上下文看不到它们 |
| [`execution_service.cpp`](../../../core/contexts/execution/application/src/execution_service.cpp)：`std::lock_guard<std::mutex> lock(cacheMutex);` | RAII | 构造时加锁，离开作用域时解锁 |
| [`domain_event.h`](../../../core/common/include/vision/common/domain_event.h)：`using EventPtr = std::shared_ptr<const DomainEvent>` | `shared_ptr` | 一个事件会分发给多个订阅者 |

在整个 `core/` 里搜索 `delete`，只会找到 `= delete`，没有一处手动释放。

## 5. 动手运行

**macOS / Linux**：

```bash
clang++ -std=c++17 -Wall -Wextra -g -fsanitize=address raii.cpp -o raii && ./raii
```

**Windows**：

```bat
cl /std:c++17 /W4 /EHsc /utf-8 raii.cpp && raii.exe
```

对照输出看每一段里“打开”和“关闭”、“加载”和“释放”是不是成对出现。

## 6. 练习

1. **观察顺序**：在 `main` 里的一个 `{ }` 里依次创建 `Camera a("A");` 和 `Camera b("B");`。离开作用域时谁先关闭？为什么这样设计是合理的？（提示：后创建的对象可能依赖先创建的对象。）
2. **写一个 RAII 类**：写 `class ScopedCounter`，构造时把一个 `int&` 加一，析构时减一。用它统计“当前有多少个函数调用正在进行中”。
3. **改造**：下面的函数有两处可能泄漏。用 `std::unique_ptr` 改写，让它在任何情况下都不泄漏。

   ```cpp
   Model* create(bool valid) {
       Model* m = new Model("x");
       if (!valid) return nullptr;      // 泄漏
       prepare(*m);                     // 如果这里抛异常，也泄漏
       return m;
   }
   ```
4. **读项目代码**：`Image` 里的像素为什么用 `shared_ptr<const ...>` 而不是 `unique_ptr`？如果改成 `unique_ptr`，`Image` 还能被复制吗？这对“图像在节点之间传递”有什么影响？

## 7. 自测题

1. 什么是 RAII？它为什么能保证资源一定被释放？
2. 函数中途抛出异常时，已经创建的局部对象会怎样？
3. `unique_ptr` 为什么不能复制？怎么把它交给另一个函数？
4. `unique_ptr` 和 `shared_ptr` 各在什么情况下用？默认该用哪个？
5. 函数参数写成 `const Model&`、`std::unique_ptr<Model>`、`std::shared_ptr<Model>`，分别表达了什么意思？
6. 现代 C++ 里裸指针还能用吗？它表示什么？

## 下一课

[第 5 课：类的设计——封装、不变量、值对象](05-class-design.md)
