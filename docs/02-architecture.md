# 02 架构设计

| 项 | 内容 |
|---|---|
| 版本 | v0.1（评审稿） |
| 状态 | 待评审；标注“待 POC”的决策由 POC 结果确定 |
| 日期 | 2026-11-11 |

## 1. 设计原则

1. **引擎与界面分离**：vision-core 是一个纯 C++ 库，不依赖任何 UI 和平台头文件，可以在命令行下独立运行和测试。
2. **一套核心，两端复用**：C++ 引擎、Web 编辑器、方案 JSON 在 Mac 和鸿蒙电脑上共用，平台相关代码限定在薄薄的适配层里。
3. **算子即插件**：新增算子不需要改框架和前端代码；前端根据算子的元数据自动生成节点外观和参数面板。
4. **先验证风险**：鸿蒙相关的不确定项都通过 POC 验证后再定稿。

## 2. 总体架构

```
┌────────────── macOS ───────────────┐   ┌──────────── 鸿蒙电脑（2in1）────────────────┐
│                                    │   │  ArkUI 原生外壳（ArkTS）                    │
│  浏览器                             │   │  ├─ 运行页面（操作员，原生）                  │
│  ┌──────────────────────────────┐  │   │  ├─ 算子测试页面（算子开发者，原生）           │
│  │ Web 编辑器 (Vue3 + TS)        │  │   │  └─ ArkWeb ┌──────────────────────────┐   │
│  │  └─ EngineClient: HttpWs     │  │   │            │ Web 编辑器（同一套代码）     │   │
│  └──────────────┬───────────────┘  │   │            │  └─ EngineClient: Bridge  │   │
│                 │ HTTP + WebSocket  │   │            └────────────┬─────────────┘   │
│  ┌──────────────▼───────────────┐  │   │                         │ JSBridge         │
│  │ vision-server (C++)           │  │   │  ┌──────────────────────▼─────────────┐   │
│  │  REST / WS → EngineService    │  │   │  │ NAPI / JSBridge 适配层 (C++)          │   │
│  └──────────────┬───────────────┘  │   │  │  → EngineService                     │   │
│                 │                  │   │  └──────────────────────┬─────────────┘   │
└─────────────────┼──────────────────┘   └─────────────────────────┼─────────────────┘
                  └──────────────────┬─────────────────────────────┘
                     ┌───────────────▼────────────────┐
                     │ EngineService（会话、方案、运行） │  ← 两端共用
                     ├────────────────────────────────┤
                     │ vision-core                     │
                     │  数据类型 · 算子注册表 · DAG 执行器 │
                     │  方案序列化 · 算子库               │
                     ├───────────────┬────────────────┤
                     │ IInferBackend │ IImageSource    │
                     │ ORT / MSLite  │ 文件 / 摄像头     │
                     └───────────────┴────────────────┘
```

**EngineService** 是两端共用的“应用服务层”：管理会话、已加载的方案、调试缓存、图像缓存。Mac 的 HTTP 服务和鸿蒙的桥接层都只是把请求翻译后转发给它，所以两端的业务逻辑只写一次。

## 3. 模块说明

| 模块 | 目录 | 语言 | 职责 | 依赖 |
|---|---|---|---|---|
| vision-core | `core/` | C++17 | 数据类型、算子接口与注册、DAG 执行、方案序列化、算子库 | OpenCV、nlohmann/json |
| EngineService | `core/service/` | C++17 | 会话、方案管理、运行控制、调试缓存、图像编码 | vision-core |
| 推理后端 | `core/infer/` | C++17 | `IInferBackend` 及 ORT / MindSpore Lite 实现 | ONNX Runtime / MindSpore Lite |
| vision-server | `server/` | C++17 | Mac 端 HTTP + WebSocket 服务 | EngineService、cpp-httplib、WS 库 |
| Web 编辑器 | `web/` | TS / Vue 3 | 节点画布、参数面板、图像查看、ROI、运行页面 | Vue Flow、Konva、Naive UI |
| 鸿蒙应用 | `harmony/` | ArkTS + C++ | 原生外壳、运行页面、算子测试页面、ArkWeb 容器、NAPI 绑定 | EngineService（交叉编译） |

## 4. 核心数据模型

### 4.1 端口数据类型

所有算子之间传递的数据都属于以下类型之一，用 `std::variant` 统一表示：

| 类型 | 说明 |
|---|---|
| `Image` | 封装 `cv::Mat`，引用计数共享，不做深拷贝；记录位深、通道数 |
| `Region` | 二值区域（掩码 + 外接框） |
| `BlobList` | Blob 列表：面积、质心、外接框、轮廓、圆度等特征 |
| `Roi` | 矩形、旋转矩形、圆、多边形 |
| `Pose` | 平移 + 旋转（+ 缩放），用于定位跟随 |
| `Point2d` / `Line2d` / `Circle2d` | 亚像素几何元素 |
| `Number` / `Bool` / `String` | 标量 |

```cpp
using Value = std::variant<Image, Region, BlobList, Roi, Pose,
                           Point2d, Line2d, Circle2d, double, bool, std::string>;
```

### 4.2 算子接口

```cpp
struct PortDef  { std::string name; PortType type; bool optional = false; };
struct ParamDef { std::string name; ParamType type; Json defaultValue; Json constraints; std::string label; };

struct OpMeta {
    std::string id;           // 例如 "vision.threshold"，全局唯一
    std::string displayName;  // "阈值"
    std::string category;     // "分割"
    std::vector<PortDef>  inputs, outputs;
    std::vector<ParamDef> params;
};

class Operator {
public:
    virtual ~Operator() = default;
    virtual const OpMeta& meta() const = 0;
    virtual Status execute(OpContext& ctx) = 0;   // ctx 提供输入、参数，接收输出
};

// 一行注册（编译期静态注册，见 ADR-05）
REGISTER_OPERATOR(ThresholdOp);
```

前端通过 `listOperators()` 拿到所有 `OpMeta`，据此自动渲染节点和参数面板。

### 4.3 DAG 执行

1. 加载方案时构图，校验端口类型和必填输入，并检测是否有环。
2. 用拓扑排序得到执行顺序（Kahn 算法）。
3. 按顺序执行每个节点：从上游节点的输出中取数据，调用 `execute`，记录耗时和状态。
4. 某个节点失败时，其下游节点标记为“跳过”，总判定为 NG（或者 ERROR，可配置）。
5. 调试模式缓存每个节点的输出；运行模式只保留终端节点的输出。
6. 并行执行（同一层中互不依赖的节点并发执行）放在 P1，POC 阶段单线程执行。

### 4.4 方案 JSON 示例

```json
{
  "schemaVersion": "0.1",
  "name": "螺丝缺陷检测-示例",
  "nodes": [
    { "id": "n1", "op": "vision.load_image", "params": {} },
    { "id": "n2", "op": "vision.to_gray",    "params": {} },
    { "id": "n3", "op": "vision.threshold",  "params": { "method": "otsu", "invert": false } },
    { "id": "n4", "op": "vision.blob",       "params": { "minArea": 50 } },
    { "id": "n5", "op": "vision.judge",      "params": { "expr": "count == 0" } }
  ],
  "edges": [
    { "from": "n1.image",  "to": "n2.image" },
    { "from": "n2.image",  "to": "n3.image" },
    { "from": "n3.region", "to": "n4.region" },
    { "from": "n4.blobs",  "to": "n5.blobs" }
  ],
  "ui": { "n1": { "x": 0, "y": 0 } }
}
```

`ui` 字段只供编辑器保存节点位置，引擎会忽略它。

## 5. 前端与引擎的通信

### 5.1 EngineClient 接口（TypeScript）

编辑器只依赖这个接口，不感知自己运行在哪个平台：

```ts
interface EngineClient {
  listOperators(): Promise<OpMeta[]>;
  loadRecipe(recipe: Recipe): Promise<ValidationResult>;
  run(input: ImageInput, opts: { debug: boolean }): Promise<RunResult>;
  getNodeOutput(nodeId: string, port: string, view?: ImageView): Promise<NodeOutput>;
  onEvent(cb: (e: EngineEvent) => void): Unsubscribe;   // 运行进度、日志
}
```

| 实现 | 平台 | 通道 |
|---|---|---|
| `HttpWsClient` | Mac（浏览器） | REST + WebSocket → vision-server |
| `BridgeClient` | 鸿蒙电脑（ArkWeb） | JSBridge → C++ 适配层（**待 POC-3b 确定**，候选方案见 03 调研 §3） |

### 5.2 图像传输策略

- 运行结果和调试预览使用**缩小并压缩后的 JPEG/PNG**（长边默认 ≤ 1600 px）。
- 放大查看时，按视口请求原图的局部区域（`ImageView = { x, y, w, h, scale }`）。
- 掩码和区域类数据以叠加层的形式传输（轮廓点或低分辨率掩码），由前端绘制。
- 鸿蒙桥接层如果不支持直接传二进制，就降级为 Base64 编码（待 POC 验证性能）。

### 5.3 Mac 端 REST 草案

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/operators` | 算子列表及元数据 |
| POST | `/api/recipes/validate` | 校验方案 |
| POST | `/api/run` | 运行（multipart：图像 + 方案 ID，或者使用当前方案） |
| GET | `/api/runs/{runId}/nodes/{nodeId}/{port}` | 获取节点输出（带视口参数） |
| WS | `/ws` | 事件推送 |

## 6. 推理后端

```cpp
class IInferBackend {
public:
    virtual Status load(const ModelSpec& spec) = 0;           // 模型路径、输入输出名、预处理配置
    virtual Status infer(const std::vector<Tensor>& in, std::vector<Tensor>& out) = 0;
    virtual std::string name() const = 0;
};
```

| 平台 | 实现 | 模型格式 | 加速 |
|---|---|---|---|
| macOS | ONNX Runtime | `.onnx` | CPU（CoreML 执行器作为可选项） |
| 鸿蒙 | MindSpore Lite | `.ms`（从 ONNX 转换） | CPU → NNRt / NPU（待 POC-4） |

模型统一维护 ONNX 一份，`models/` 目录下提供转换脚本。预处理（缩放、归一化）放在 C++ 算子里实现，保证两端一致。

## 7. 构建与交叉编译

| 目标 | 工具链 | 说明 |
|---|---|---|
| Mac | CMake + AppleClang；依赖通过 Homebrew 安装 | `cmake --preset mac` |
| 鸿蒙 | 鸿蒙 NDK 自带的 CMake 和 Clang，目标 `arm64-v8a` | 第三方库（OpenCV 等）用 lycium 框架或自行交叉编译，详见 03 调研 §2 |
| Web | Vite | Mac 端由 vision-server 托管静态文件；鸿蒙端打包进 App 的 rawfile |

**算子在两端共用同一份源码**，鸿蒙工程的 `CMakeLists.txt` 直接引用 `core/`。

## 8. 架构决策记录（ADR）

| 编号 | 决策点 | 选择 | 备选方案 | 理由 | 状态 |
|---|---|---|---|---|---|
| ADR-01 | 核心语言 | C++17 | C++20、Rust | 两端编译器都支持得好；团队熟悉 | 已定 |
| ADR-02 | 构建 | CMake | Bazel、xmake | 鸿蒙 NDK 原生支持 | 已定 |
| ADR-03 | 图像库 | OpenCV 4（裁剪模块） | 全部自研 | 生态成熟，lycium 有现成配方；核心算法另外手写一份用于学习和对照 | 待 POC-3a |
| ADR-04 | 流程模型 | DAG + 拓扑排序 | 状态机、脚本 | 直观，适合可视化 | 已定 |
| ADR-05 | 算子扩展 | 编译期静态注册 | 动态加载 .so | 鸿蒙沙箱限制动态加载；两端用同一套机制 | 已定 |
| ADR-06 | 方案格式 | JSON + `schemaVersion` | YAML、protobuf | 前端、C++、ArkTS 都能直接读写 | 已定 |
| ADR-07 | 编辑器 | Web：Vue 3 + TS + Vite + Vue Flow + Konva + Naive UI | Qt、React、ArkUI 原生 | 一套代码两端复用；美观；与 ArkTS 语法相近 | 已定 |
| ADR-08 | 鸿蒙端 UI | ArkUI 原生外壳 + ArkWeb 嵌入编辑器 | 纯 WebView 套壳、纯原生 | 兼顾复用与原生开发能力（比赛看重原生） | 已定 |
| ADR-09 | Mac 端通信 | 本地 HTTP + WebSocket | Electron + Node 插件、Tauri | 不用打包，浏览器刷新即可，迭代最快 | 已定 |
| ADR-10 | 鸿蒙端通信 | ArkWeb JSBridge（C++ 侧直接注册，或经过 ArkTS） | App 进程内起本地 HTTP 服务 | 官方支持的机制 | **待 POC-3b** |
| ADR-11 | 推理 | `IInferBackend`：ORT / MindSpore Lite | 两端都用 ORT | 鸿蒙上 MindSpore Lite 是系统能力，可以用 NPU | 待 POC-4 |
| ADR-12 | 测试 | GoogleTest + 图像回归测试 | Catch2 | 主流 | 已定 |
| ADR-13 | 仓库 | 单仓库 | 多仓库 | 小团队管理简单 | 已定 |
| ADR-14 | 鸿蒙设备类型 | `2in1` 为主，兼容 `tablet` | 只做 `2in1` | 平板可以用于拍照演示 | 已定 |
| ADR-15 | 编辑器交互 | 键鼠优先，触屏适配为 P2 | 触屏优先 | 主目标是电脑 | 已定 |

## 9. 目录结构

```
vision-platform/
├── core/
│   ├── include/vision/     # 公共头文件：types.h, operator.h, graph.h, recipe.h
│   ├── src/                # 执行器、注册表、序列化
│   ├── ops/                # 算子库（每个算子一个 .cpp）
│   ├── service/            # EngineService
│   ├── infer/              # 推理后端
│   └── tests/
├── server/                 # vision-server（Mac）
├── cli/                    # 命令行运行器：vision-run recipe.json image.png
├── web/                    # Vue 3 编辑器
├── harmony/                # DevEco 工程
│   └── entry/src/main/
│       ├── ets/            # ArkTS 页面
│       ├── cpp/            # NAPI / JSBridge 适配层，CMake 引用 ../../core
│       └── resources/rawfile/web/   # 打包好的 Web 编辑器
├── models/                 # ONNX 模型、转换脚本
├── samples/                # 示例图像、示例方案
├── third_party/            # 交叉编译脚本和产物说明（不提交二进制）
└── docs/
```
