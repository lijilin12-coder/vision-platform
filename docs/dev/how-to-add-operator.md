# 如何新增一个算子

> 目标：照着这份文档，1 天内完成一个新算子（NFR-10）。
> 前置知识：C++ 基础（类、虚函数）。不需要懂 DDD，也不需要懂整个平台。

## 1. 算子是什么

**算子**是一种算法能力，比如“阈值”“Blob 分析”。它有：

- **输入端口**：接收上游节点的数据，比如一张图像
- **输出端口**：产出数据给下游节点，比如一个区域
- **参数**：用户可以调节的值，比如阈值大小

用户在编辑器里把算子拖进方案，就生成了一个**节点**。同一个算子可以在一个方案里出现很多次，每个节点有自己的参数（术语表 §2、§3）。

端口能传递的数据类型（`PortType`）：

| 类型 | C++ 类型 | 含义 |
|---|---|---|
| `Image` | `vision::Image` | 图像 |
| `Region` | `vision::Region` | 算法计算出的像素集合（掩码） |
| `Blobs` | `vision::BlobList` | 连通块列表 |
| `Number` | `double` | 数值 |
| `Bool` | `bool` | 布尔值 |
| `Text` | `std::string` | 文本 |
| `Judgement` | `vision::Judgement` | 判定依据：合格与否 + 原因 |

## 2. 三步写一个算子

在 `operators/src/` 下**新建一个 `.cpp` 文件**就够了，不需要修改任何其他文件。完整例子见 [`operators/src/invert.cpp`](../../operators/src/invert.cpp)（反色算子）。

```cpp
#include <opencv2/core.hpp>
#include "vision/catalog/api.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::operators {

class InvertOp final : public catalog::Operator {
public:
    // 第 1 步：元数据
    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.invert";          // 全局唯一，形如“分类.名称”
        m.signature.majorVersion = 1;               // 主版本号（见第 5 节）
        m.signature.inputs  = {{"image", PortType::Image, false}};   // 名称、类型、是否可选
        m.signature.outputs = {{"image", PortType::Image, false}};
        m.displayName = "反色";
        m.category = "预处理";
        return m;
    }

    // 第 2 步：执行
    Status execute(catalog::OpContext& ctx) override {
        auto in = ctx.input<Image>("image");        // 读输入
        if (!in) return in.error();                 // 类型不对或缺失时返回错误
        cv::Mat out;
        cv::bitwise_not(imaging::view(in.value()), out);   // 用 OpenCV 计算
        auto img = imaging::fromMat(out);
        if (!img) return img.error();
        ctx.setOutput("image", img.value());        // 写输出
        return Status::success();
    }
};

}  // namespace vision::operators

// 第 3 步：注册（一行）
using vision::operators::InvertOp;
VISION_REGISTER_OPERATOR(InvertOp)
```

## 3. 参数

在 `meta()` 里定义参数，平台会自动检查用户输入的值是否合法（类型、范围、选项）：

```cpp
m.signature.params = {
    // 名称      类型                默认值                最小值        最大值        选项              说明
    {"value",  ParamType::Number, 128.0,                0.0,          255.0,        {},               "阈值"},
    {"invert", ParamType::Bool,   false,                std::nullopt, std::nullopt, {},               "取暗区域"},
    {"method", ParamType::Choice, std::string("fixed"), std::nullopt, std::nullopt, {"fixed", "otsu"}, "方法"},
    {"label",  ParamType::Text,   std::string("缺陷"),  std::nullopt, std::nullopt, {},               "名称"},
};
```

在 `execute()` 里读取参数：

```cpp
double v = ctx.number("value");
bool inv = ctx.flag("invert");
const std::string& method = ctx.text("method");   // Choice 和 Text 都用 text()
```

> ⚠️ 文本默认值要写成 `std::string("...")`，不能直接写 `"..."`。直接写字符串字面量时，C++ 会把它当成 `bool`。

## 4. 常用工具

| 需要 | 写法 |
|---|---|
| 把 `Image` 交给 OpenCV（不拷贝像素，只读） | `cv::Mat m = imaging::view(image);` |
| 把 OpenCV 结果变回 `Image` | `auto img = imaging::fromMat(mat);` |
| 生成区域 | `auto region = Region::fromMask(maskImage);`（掩码必须是单通道） |
| 读 ROI（用户可能没设置） | `if (const auto& roi = ctx.roi()) { roi->x(); ... }` |
| 输出判定依据 | `ctx.setOutput("judgement", Judgement{pass, "原因说明"});` |

**判定依据很重要**：方案至少要有一个节点输出 `Judgement`，否则不能发布（发布规则 P5）。原因说明要让操作员看得懂，比如“亮斑 2 处，上限 0 处”，而不是“count=2”。

## 5. 主版本号

算子的主版本号表示**兼容性**（D5-1）：

- 修 bug、提升速度、增加一个**有默认值**的参数 → 主版本号**不变**
- 删除或改名端口、删除参数、改变参数含义 → 主版本号 **+1**

已发布的方案记录了它用的算子主版本号。主版本号变了，旧方案需要方案开发者执行“升级节点”后重新发布。

## 6. 编译、试用、测试

```bash
cmake --build build                       # 新文件会被自动发现
./build/apps/vision-cli operators         # 应该能看到你的算子
```

在方案里试用：把 `samples/recipes/bright-spot.draft.json` 复制一份，加入你的算子，然后：

```bash
./build/apps/vision-cli recipe build 你的草稿.json         # 输出方案 ID
./build/apps/vision-cli recipe debug <方案ID> 某张图.png    # 看每个节点的输出
```

建议给算子写一个测试，放在 `tests/integration_test.cpp` 里，参考已有的测试。

## 7. 检查清单

- [ ] 算子 ID 形如“分类.名称”，全局唯一
- [ ] 每个输出端口在 `execute()` 成功时都设置了值（否则执行引擎会判为失败）
- [ ] 出错时返回 `Status::failure(...)`，并写清楚原因
- [ ] 参数有合理的默认值和范围
- [ ] 如果输出判定依据，原因说明让操作员看得懂
- [ ] `vision-cli operators` 能看到它，`recipe debug` 能跑通
