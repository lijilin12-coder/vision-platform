# 第 11 课：单元测试——GoogleTest

> 预计用时：1.5 小时
> 示例：这一课不另写示例，直接使用项目自己的测试，在 [`tests/`](../../../tests/) 目录下。需要先按 [构建说明](../../dev/build.md) 把项目编译好。

## 这一课要回答的问题

前面每一课的示例里都有一个手写的 `check(条件, "说明")`。真正的项目里用测试框架。

1. 为什么要写测试？测什么？
2. GoogleTest 怎么用？
3. 怎么让代码“好测”？

## 1. 为什么要写测试

竞赛里，评测机就是你的测试。工程里没有评测机，得自己造一个。

| 没有测试 | 有测试 |
|---|---|
| 改完代码，手动运行几次，看着差不多就提交 | 改完代码，运行一条命令，一秒钟后知道有没有改坏 |
| 不敢动别人写的代码，怕改坏了不知道 | 放心重构，测试通过就说明行为没变 |
| bug 修好之后，过几个月又被别的改动带回来 | 每个修过的 bug 都留下一个测试，再也回不来 |
| 换个平台能不能用，要手动试 | 自动构建在 Windows 和 Linux 上各跑一遍 |

本项目目前有 43 个测试，全部运行一遍不到 1 秒。POC-1 里那个算子注册的 bug（第 3 课），就是集成测试在几分钟内抓到的。

## 2. 第一个测试

打开 [`tests/kernel_test.cpp`](../../../tests/kernel_test.cpp)：

```cpp
#include <gtest/gtest.h>

#include "vision/kernel/geometry.h"

using namespace vision;

TEST(Roi, RejectsNonPositiveSize) {
    EXPECT_FALSE(Roi::rect(0, 0, 0, 10).ok());
    EXPECT_FALSE(Roi::rect(0, 0, 10, -1).ok());
    EXPECT_TRUE(Roi::rect(5, 5, 10, 10).ok());
}
```

- `TEST(测试组, 测试名)` 定义一个测试。两个名字合起来说明“测的是什么、期望什么行为”。
- `EXPECT_TRUE`、`EXPECT_FALSE` 是断言：条件不符合预期时，这个测试失败，并报告是哪一行。
- 不需要写 `main`。链接 `GTest::gtest_main` 就自动有了。

> 测试文件里写了 `using namespace vision;`。测试代码不是头文件，不会被别人包含，这样写可以接受。

### 常用断言

| 断言 | 检查 |
|---|---|
| `EXPECT_TRUE(x)` / `EXPECT_FALSE(x)` | 真 / 假 |
| `EXPECT_EQ(a, b)` / `EXPECT_NE(a, b)` | 相等 / 不等 |
| `EXPECT_LT`、`EXPECT_LE`、`EXPECT_GT`、`EXPECT_GE` | 小于、小于等于、大于、大于等于 |
| `EXPECT_DOUBLE_EQ(a, b)` | 两个 `double` 几乎相等 |
| `EXPECT_NEAR(a, b, 误差)` | 两个数相差不超过给定的误差 |
| `EXPECT_STREQ(a, b)` | 两个 C 字符串内容相同 |

**浮点数不要用 `EXPECT_EQ` 比较**计算结果，用 `EXPECT_NEAR`。

### `EXPECT` 和 `ASSERT`

每个 `EXPECT_xxx` 都有对应的 `ASSERT_xxx`：

- `EXPECT`：失败后**继续**执行这个测试，可以一次看到多处失败。
- `ASSERT`：失败后**立刻结束**这个测试。

后面的代码依赖前面的结果时，用 `ASSERT`：

```cpp
auto v = recipe.publish();
ASSERT_TRUE(v.ok()) << v.error().message;     // 不成功就别往下走了，否则下一行会崩溃
EXPECT_EQ(v.value().id().str(), "r-1@v1");
```

`<<` 后面可以追加说明，失败时会一起打印出来。

## 3. 测什么：一个测试只验证一件事

打开 [`tests/authoring_domain_test.cpp`](../../../tests/authoring_domain_test.cpp)。测试的名字直接对应设计文档里的规则编号：

```cpp
TEST(Recipe, R1_RejectsCycle) { ... }
TEST(Recipe, R2_RejectsTypeMismatchAndMissingPorts) { ... }
TEST(Recipe, R3_OneEdgePerInput) { ... }
TEST(Recipe, R4_ValidatesParams) { ... }
```

看其中一个：

```cpp
TEST(Recipe, R1_RejectsCycle) {
    Recipe r = Recipe::create("r-1", "测试");            // 准备
    auto a = r.addNode(test::gray()).value();
    auto b = r.addNode(test::gray()).value();
    auto c = r.addNode(test::gray()).value();
    ASSERT_TRUE(r.connect(port(a + ".image"), port(b + ".image")).ok());
    ASSERT_TRUE(r.connect(port(b + ".image"), port(c + ".image")).ok());

    auto st = r.connect(port(c + ".image"), port(a + ".image"));   // 执行：连一条会成环的线

    ASSERT_FALSE(st.ok());                                // 检查
    EXPECT_EQ(st.error().code, "authoring.cycle");
    EXPECT_FALSE(r.connect(port(a + ".image"), port(a + ".image")).ok()) << "自己连自己也是环";
}
```

好的测试有固定的三段：**准备 → 执行 → 检查**。

几个值得学的地方：

- **检查的是错误码，不只是“失败了”。** 如果只检查 `!st.ok()`，那么因为别的原因失败（比如节点名写错了）测试也会通过，其实什么都没验证。
- **正常情况和异常情况都要测。** 每条规则至少一个“应该被拒绝”的测试。
- **边界情况单独测。** “自己连自己”是最小的环。
- **被拒绝之后状态不变。** 看 `R4_ValidatesParams` 的最后一行：设置非法参数被拒绝后，参数仍然是原来的值。

### 该测什么

| 值得测 | 不值得测 |
|---|---|
| 业务规则（不变量、发布规则） | 只是转发调用的代码 |
| 边界：空、零、负数、最大值、刚好相等 | 标准库和第三方库 |
| 每个修过的 bug | 没有逻辑的 getter |
| 错误路径：非法输入、文件不存在 | |

## 4. 测试夹具：多个测试共用的准备工作

多个测试需要相同的准备时，用 `TEST_F`。打开 [`tests/integration_test.cpp`](../../../tests/integration_test.cpp)：

```cpp
class Platform : public ::testing::Test {
protected:
    fs::path dir = ...;                              // 每个测试一个独立的临时目录
    eventbus::InMemoryEventBus bus;
    persistence::JsonRecipeRepository recipes{dir};
    authoring::AuthoringService authoring{...};

    void SetUp() override { ... }                    // 每个测试开始前运行
    void TearDown() override { fs::remove_all(dir); } // 每个测试结束后运行：清理临时文件
};

TEST_F(Platform, EndToEndInspection) {
    auto rid = buildRecipe();          // 直接使用夹具里的成员
    ...
}
```

**每个测试都会得到一个全新的夹具对象。** 测试之间互不影响，可以按任意顺序运行，也可以只运行其中一个。

## 5. 让代码好测

有些代码很难测试，原因通常是它依赖了测试里不方便提供的东西。前面几课学的做法，都在让代码变得好测：

| 难测的原因 | 做法 | 课 |
|---|---|---|
| 读写全局变量、`cin`、`cout` | 输入从参数进，结果从返回值出 | 第 0 课 |
| 依赖相机、文件、数据库 | 依赖接口，测试时换成假实现 | 第 8 课 |
| 逻辑和技术细节混在一起 | 业务规则放在不依赖任何技术的领域层 | 第 5 课 |

看 [`tests/execution_domain_test.cpp`](../../../tests/execution_domain_test.cpp) 里的 `FakeRunner`。它代替了真实的算子，所以：

- 测试不需要图像，也不链接 OpenCV；
- 可以随意指定“这次检测出 3 个缺陷”“阈值算子执行失败”，这些情况用真实算子很难稳定地制造出来。

```cpp
TEST(Executor, FailureSkipsDownstreamAndGivesError) {
    FakeFactory f;
    f.failOperator = "test.threshold";       // 让阈值算子失败
    ...
    EXPECT_EQ(ex[2].status, NodeStatus::Failed);
    EXPECT_EQ(ex[3].status, NodeStatus::Skipped);
    EXPECT_EQ(i.verdict().kind, VerdictKind::Error);
}
```

### 单元测试和集成测试

| | 单元测试 | 集成测试 |
|---|---|---|
| 测什么 | 一个类或几个类的逻辑 | 多个模块组装起来的完整流程 |
| 依赖 | 假实现，不碰文件和硬件 | 真实的算子、OpenCV、文件 |
| 速度 | 毫秒 | 稍慢 |
| 项目里 | `kernel_tests`、`catalog_tests`、`authoring_domain_tests`、`execution_domain_tests` | `integration_tests` |

两种都需要。单元测试多而快，覆盖各种规则和边界；集成测试少一些，确认各部分接在一起确实能工作。

## 6. 运行测试

在项目根目录下（Windows 上把 `default` 换成 `windows`）：

```bash
ctest --preset default                          # 运行全部测试
```

```bash
ctest --preset default -R Recipe                # 只运行名字里带 Recipe 的测试
```

直接运行某个测试程序，输出更详细，也可以筛选：

```bash
./build/tests/authoring_domain_tests --gtest_filter='Recipe.R1*'
```

失败时的输出会指出文件、行号、实际值和期望值，形如：

```
tests/authoring_domain_test.cpp:68: Failure
Expected equality of these values:
  st.error().code
    Which is: "authoring.type_mismatch"
  "authoring.cycle"
```

### 测试是怎么加进构建的

[`tests/CMakeLists.txt`](../../../tests/CMakeLists.txt)：

```cmake
add_executable(authoring_domain_tests authoring_domain_test.cpp)
target_link_libraries(authoring_domain_tests PRIVATE authoring_domain GTest::gtest_main)
gtest_discover_tests(authoring_domain_tests)
```

注意第二行：这个测试只链接了 `authoring_domain` 和 GoogleTest，**没有 OpenCV**。领域层不依赖任何技术，所以它的测试也不依赖。

## 7. 一个特殊的测试：编译失败才算通过

[`tests/boundary/violation.cpp`](../../../tests/boundary/violation.cpp) 故意让方案编排的代码去包含检测执行的领域头文件。对应的测试会尝试编译它，并且**要求编译失败**，还要求失败的原因确实是找不到那个头文件。

这个测试保护的不是某个函数的行为，而是架构本身：上下文之间的边界不能被悄悄打破。

## 8. 修 bug 的正确顺序

1. **先写一个能重现这个 bug 的测试**，运行，确认它失败。
2. 修改代码。
3. 运行测试，确认它通过，同时其他测试没有被弄坏。

第 1 步很重要：它证明了这个测试确实能抓到这个 bug。算子注册那个 bug 修复时补的 [`tests/catalog_test.cpp`](../../../tests/catalog_test.cpp) 就是这样来的，注释里写明了它防的是什么。

## 9. 练习

1. **运行并读懂**：运行全部测试。然后只运行 `Recipe` 相关的测试。打开 `authoring_domain_test.cpp`，找到 `R6_RemoveNodeRemovesItsEdges`，说出它的“准备、执行、检查”分别是哪几行。
2. **让一个测试失败**：把 `recipe.cpp` 里 `Recipe::connect` 检查“输入端口已被占用”的那段代码注释掉，重新编译并运行测试。哪些测试失败了？失败信息告诉了你什么？改回去。
3. **写一个新测试**：在 `authoring_domain_test.cpp` 里增加 `TEST(Recipe, DisconnectRemovesEdge)`：搭好流程后断开其中一条线，检查连线数量少了一条；再次断开同一条线，检查返回的错误码是 `authoring.edge_not_found`。
4. **给算子写测试**：参考 `integration_test.cpp`，为 `vision.invert`（反色）算子写一个测试：构造一张已知像素值的小图像，经过“图像输入 → 反色”后，检查输出像素等于 255 减去原值。提示：用 `execution.debugRun` 可以拿到每个节点的输出。
5. **先写测试再修 bug**：`Recipe::defineNamedRegion` 目前允许名字只有空格的具名区域（比如 `"   "`）。先写一个期望它被拒绝的测试，确认测试失败，再修改代码让测试通过。

## 10. 自测题

1. 测试带来的最大好处是什么？
2. `EXPECT_EQ` 和 `ASSERT_EQ` 的区别是什么？各自什么时候用？
3. 一个好的测试分哪三段？
4. 测试“连线被拒绝”时，为什么要检查错误码，而不是只检查“失败了”？
5. `TEST_F` 的夹具对象，是所有测试共用一个，还是每个测试一个？这样设计有什么好处？
6. 领域层的测试为什么可以不链接 OpenCV？
7. 修 bug 时，为什么要先写一个会失败的测试？

## 下一课

[第 12 课：调试技巧](12-debugging.md)
