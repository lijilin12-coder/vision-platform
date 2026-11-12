# 07 架构图：模块关系、消息传递、数据流向

| 项 | 内容 |
|---|---|
| 版本 | v0.1 |
| 日期 | 2026-11-12 |
| 依据 | [02-architecture.md](02-architecture.md)、[ddd/04-context-map.md](ddd/04-context-map.md)、[ddd/05-tactical-design.md](ddd/05-tactical-design.md)，以及 POC-1 的实际代码 |

本文档用 8 张图回答三个问题：

| 问题 | 图 |
|---|---|
| 模块之间是什么关系？ | 图 1（总览）、图 2（代码模块依赖） |
| 消息怎么传递？ | 图 3（上下文之间的消息）、图 4（编辑与发布时序）、图 5（一次检测的时序）、图 7（产线热切换时序） |
| 数据怎么流动？ | 图 6（一次检测内部的数据流）、图 8（数据到模型的闭环） |

**图例（全文通用）**

| 画法 | 含义 |
|---|---|
| 实线箭头 `A --> B` | 同步调用：A 调用 B 的公开接口，等待返回 |
| 虚线箭头 `A -.-> B` | 领域事件：A 发布事件，B 订阅；A 不知道 B 的存在 |
| 粗线箭头 `A ==> B` | 发布语言：带版本号的文档或文件（JSON、方案包、上线包文件、模型制品） |
| 绿色 | POC-1 已实现 |
| 灰色 | 已设计，尚未实现 |

---

## 图 1：系统总览

三层结构：驱动适配器（谁来调用）→ 核心（10 个限界上下文中的 9 个）→ 被驱动适配器（核心调用谁）。模型工具链是独立进程。

```mermaid
flowchart TB
    subgraph DRV["驱动适配器（入口）"]
        CLI["vision-cli<br/>命令行"]:::done
        RT["vision-runtime<br/>HTTP + WebSocket<br/>Linux / Windows"]:::plan
        HM["鸿蒙应用<br/>NAPI / ArkWeb 桥接"]:::plan
    end

    subgraph CORE["核心（C++17，各平台完全相同）"]
        direction TB
        subgraph CORE_DOM["核心域"]
            AUT["BC-2 方案编排"]:::done
            EXE["BC-3 检测执行"]:::done
            REL["BC-4 上线管理"]:::plan
            STA["BC-5 工位运行"]:::plan
        end
        subgraph SUP_DOM["支撑域"]
            CAT["BC-1 算子目录"]:::done
            CAL["BC-6 标定"]:::plan
            MOD["BC-7 模型仓库"]:::plan
            DAT["BC-9 数据"]:::plan
            QUA["BC-10 质量分析"]:::plan
        end
        KER["共享内核 kernel<br/>Image · Region · Roi · 算子签名"]:::done
        OPS["算子 operators"]:::done
    end

    subgraph DVN["被驱动适配器（技术细节）"]
        PER["持久化<br/>JSON 文件 · SQLite · 内容寻址存储"]:::done
        IMG["图像<br/>OpenCV 桥接 · 图像源"]:::done
        BUS["事件总线"]:::done
        INF["推理<br/>ONNX Runtime · MindSpore Lite"]:::plan
        EXC["包交换<br/>方案包 · 上线包文件"]:::plan
        TCC["工具链客户端"]:::plan
    end

    TCH["BC-8 模型工具链<br/>独立 Linux 服务（Docker）"]:::plan

    DRV --> CORE
    CORE --> DVN
    TCC ==> TCH

    classDef done fill:#d5f5d5,stroke:#2e7d32,color:#1b1b1b
    classDef plan fill:#eeeeee,stroke:#9e9e9e,color:#555555
```

---

## 图 2：代码模块依赖（POC-1 的实际 CMake 目标）

箭头方向是“依赖”。**点线**表示私有依赖：应用层私有地链接自己的领域层，所以领域层的头文件不会传递给使用者，其他上下文想包含它会编译失败（ADR-17）。

```mermaid
flowchart TB
    CLI["vision-cli<br/>组装根"]:::app

    subgraph APPL["应用层（每个上下文对外只暴露 api.h）"]
        EXA["execution_application"]:::appl
        AUA["authoring_application"]:::appl
        CAA["catalog_application"]:::appl
    end

    subgraph DOMN["领域层（只能依赖 kernel 和 common）"]
        EXD["execution_domain"]:::dom
        AUD["authoring_domain"]:::dom
        CAD["catalog_domain"]:::dom
    end

    KER["vision_kernel<br/>共享内核"]:::ker
    COM["vision_common<br/>Result · ID · 事件基类"]:::ker

    subgraph ADP["适配器与算子"]
        OPS["vision_operators"]:::adp
        IMG["vision_imaging"]:::adp
        PER["vision_persistence"]:::adp
        BUS["vision_eventbus"]:::adp
    end

    CV(["OpenCV"]):::ext
    NJ(["nlohmann-json"]):::ext

    CLI --> EXA & AUA & CAA & OPS & IMG & PER & BUS

    EXA -->|"只用发布语言<br/>RecipeVersionDocument"| AUA
    EXA -->|"创建算子实例"| CAA
    AUA -->|"取算子签名快照"| CAA

    EXA -.->|私有| EXD
    AUA -.->|私有| AUD
    CAA -.->|私有| CAD

    EXD & AUD & CAD --> KER
    KER --> COM

    OPS --> CAA
    OPS --> IMG
    IMG --> KER
    IMG --> CV
    PER -->|"实现仓储接口"| AUD
    PER --> NJ
    BUS --> COM

    classDef app fill:#fff3cd,stroke:#b8860b,color:#1b1b1b
    classDef appl fill:#d6e9ff,stroke:#1565c0,color:#1b1b1b
    classDef dom fill:#ffd9d9,stroke:#c62828,color:#1b1b1b
    classDef ker fill:#e6dcff,stroke:#5e35b1,color:#1b1b1b
    classDef adp fill:#d5f5d5,stroke:#2e7d32,color:#1b1b1b
    classDef ext fill:#eeeeee,stroke:#9e9e9e,color:#555555
```

要点：

- **领域层（红色）不依赖 OpenCV**，也不依赖任何适配器。
- **检测执行不认识方案编排的领域类**，只认识它的发布语言 `RecipeVersionDocument`。
- **适配器依赖领域层**（实现领域层定义的接口），而不是反过来。这就是依赖倒置。
- 只有组装根 `vision-cli` 能同时看到所有模块。

---

## 图 3：上下文之间的消息

线上的文字是具体的消息名。三种线型对应三种传递方式（见图例）。

```mermaid
flowchart TB
    CAT["BC-1<br/>算子目录"]:::done
    AUT["BC-2<br/>方案编排"]:::done
    EXE["BC-3<br/>检测执行"]:::done
    REL["BC-4<br/>上线管理"]:::plan
    STA["BC-5<br/>工位运行"]:::plan
    CAL["BC-6<br/>标定"]:::plan
    MOD["BC-7<br/>模型仓库"]:::plan
    TCH["BC-8<br/>模型工具链"]:::plan
    DAT["BC-9<br/>数据"]:::plan
    QUA["BC-10<br/>质量分析"]:::plan
    TRAIN(["外部训练"]):::ext
    OTHER(["其他机器"]):::ext
    CAM(["相机 / 文件夹"]):::ext

    AUT -->|"find 算子签名"| CAT
    EXE -->|"create 算子实例"| CAT
    AUT -->|"debugRun 草稿文档"| EXE
    AUT ==>|"方案版本文档 JSON"| REL
    AUT -.->|"RecipeVersionPublished"| REL
    MOD -.->|"ModelVersionPublished"| REL
    REL -->|"查询模型契约和制品"| MOD
    REL -->|"inspect 验收与一致性检查"| EXE
    REL ==>|"上线包清单"| STA
    REL <==>|"方案包 · 上线包文件"| OTHER
    CAL -->|"标定档案"| STA
    CAM ==>|"图像帧"| STA
    STA -->|"inspect 产线检测"| EXE
    EXE -.->|"InspectionCompleted"| STA
    EXE -.->|"InspectionCompleted"| DAT
    EXE -.->|"InspectionCompleted"| QUA
    STA -.->|"RolledBack 请求撤回"| REL
    STA -.->|"HotSwapped · CalibrationAssigned"| QUA
    QUA -.->|"MisjudgmentConfirmed"| DAT
    DAT ==>|"数据集导出"| TRAIN
    TRAIN ==>|"ONNX + 元数据"| MOD
    MOD -->|"量化 · 转换 · 验证任务"| TCH
    TCH ==>|"模型制品 · 验证报告"| MOD

    classDef done fill:#d5f5d5,stroke:#2e7d32,color:#1b1b1b
    classDef plan fill:#eeeeee,stroke:#9e9e9e,color:#555555
    classDef ext fill:#ffffff,stroke:#9e9e9e,color:#555555,stroke-dasharray: 4 3
```

### 领域事件一览

| 事件 | 发布者 | 订阅者及其动作 | 状态 |
|---|---|---|---|
| `RecipeCreated` | BC-2 `Recipe` | 界面刷新 | ✅ 已实现 |
| `RecipeVersionPublished` | BC-2 `Recipe` | BC-4：可以用它创建上线包 | ✅ 已发布，订阅者待实现 |
| `RecipeVersionDeprecated` | BC-2 `RecipeVersion` | BC-4：不能再用它创建上线包 | ✅ 已发布，订阅者待实现 |
| `InspectionCompleted` | BC-3 `Inspection`（调试运行不发布） | BC-5：统计连续 ERROR、NG 率；BC-9：图像缓存、按采集策略存样本；BC-10：检测记录、批次统计 | ✅ 已发布，订阅者待实现 |
| `ModelVersionPublished` | BC-7 | BC-4：提示可以创建新上线包（规则 P9） | 待实现 |
| `ReleaseCreated` / `ReleaseAccepted` / `ReleaseConsistencyPassed` / `ReleaseWithdrawn` | BC-4 `Release` | 界面；BC-5 部署检查时查询状态 | 待实现 |
| `ReleaseDeployed` / `ReleaseReady` / `HotSwapped` / `RolledBack` | BC-5 `Station` | 界面实时刷新；BC-10 在趋势图上标注；`RolledBack` 带撤回请求时 BC-4 撤回上线包 | 待实现 |
| `QualityDeviationAlerted` / `CalibrationExpired` | BC-5 `Station` | 界面告警 | 待实现 |
| `MisjudgmentConfirmed` | BC-10 `Review` | BC-9：样本进入待标注；漏检时从图像缓存找回 | 待实现 |

---

## 图 4：编辑并发布方案的消息时序（POC-1 已实现）

注意：被拒绝的命令直接返回错误，**不产生事件**；编辑草稿也不产生事件，只有“创建”和“发布”会发布事件。

```mermaid
sequenceDiagram
    autonumber
    actor U as 方案开发者
    participant CLI as vision-cli
    participant AS as AuthoringService<br/>应用层
    participant R as Recipe 聚合<br/>领域层
    participant CAT as OperatorCatalog
    participant REPO as JSON 仓储
    participant BUS as 事件总线

    U->>CLI: recipe add-node 阈值
    CLI->>AS: addNode(recipeId, operatorId)
    AS->>CAT: find(operatorId)
    CAT-->>AS: 算子签名快照
    AS->>REPO: find(recipeId)
    REPO-->>AS: Recipe
    AS->>R: addNode(签名)
    R-->>AS: 节点 ID
    AS->>REPO: save(Recipe)
    AS-->>CLI: 节点 ID

    U->>CLI: recipe connect n5.judgement n2.image
    CLI->>AS: connect(...)
    AS->>R: connect(from, to)
    Note over R: 检查 R1 无环、R2 类型兼容、R3 单入线
    R-->>AS: 错误 type_mismatch
    AS-->>CLI: 错误（不保存，不发事件）

    U->>CLI: recipe publish
    CLI->>AS: publish(recipeId)
    AS->>R: publish()
    Note over R: 检查 P1 必填输入已连接、P5 有判定依据
    R-->>AS: RecipeVersion 聚合（不可变快照）
    AS->>REPO: save(RecipeVersion)
    AS->>REPO: save(Recipe)
    AS->>BUS: RecipeVersionPublished
    AS-->>CLI: 方案版本 ID
```

---

## 图 5：一次检测的消息时序（POC-1 已实现）

```mermaid
sequenceDiagram
    autonumber
    participant CLI as vision-cli
    participant AS as AuthoringService
    participant ES as ExecutionService<br/>应用层
    participant PL as ExecutionPlanner<br/>领域服务
    participant EX as Executor<br/>领域服务
    participant OP as 算子<br/>经 NodeRunner 端口
    participant INS as Inspection 聚合
    participant J as Judge<br/>领域服务
    participant BUS as 事件总线

    CLI->>AS: getVersionDocument(versionId)
    AS-->>CLI: RecipeVersionDocument（发布语言）
    CLI->>ES: inspect(文档, 图像)
    Note over ES: 发布语言 → 本上下文的 PlanSpec<br/>检查算子存在、主版本号匹配
    ES->>PL: plan(PlanSpec)
    Note over PL: 拓扑排序、绑定输入输出、创建节点运行器
    PL-->>ES: ExecutionPlan（按方案版本缓存）
    ES->>INS: start(计划的节点顺序)
    ES->>EX: run(计划, 图像, Inspection)
    loop 按拓扑顺序执行每个节点
        EX->>OP: run(输入值, 参数, ROI)
        OP-->>EX: 输出值，或错误
        EX->>INS: recordNode(状态, 耗时)
        Note over EX: 上游失败时，本节点记为跳过
    end
    EX->>J: judge(节点执行记录, 判定依据)
    J-->>EX: Verdict：OK / NG / ERROR
    EX->>INS: complete(Verdict)
    Note over INS: 检查 I2、I3、I4
    ES->>BUS: InspectionCompleted（调试运行不发布）
    ES-->>CLI: InspectionReport：判定、判定依据、各节点耗时
```

---

## 图 6：一次检测内部的数据流（示例方案，POC-1 已实现）

线上标注的是端口上流动的**数据类型**。判定依据（`Judgement`）汇总成判定后，随事件流向下游。

```mermaid
flowchart LR
    FILE[("图像文件")]
    FILE ==>|"imaging::loadImage"| FRAME["Image<br/>BGR 640×480"]

    subgraph PLAN["ExecutionPlan（示例：亮斑缺陷检测）"]
        direction LR
        N1["n1 图像输入"]
        N2["n2 灰度"]
        N3["n3 阈值<br/>value=200<br/>ROI 矩形"]
        N4["n4 Blob 分析<br/>minArea=20"]
        N5["n5 计数判定<br/>maxCount=0"]
        N1 -->|"Image 彩色"| N2
        N2 -->|"Image 灰度"| N3
        N3 -->|"Region 掩码"| N4
        N4 -->|"BlobList"| N5
    end

    FRAME ==> N1
    N5 -->|"Judgement<br/>亮斑 1 处，上限 0 处"| JUDGE["Judge 汇总"]
    JUDGE --> VERDICT["Verdict = NG<br/>+ 判定依据"]
    VERDICT --> REPORT["InspectionReport<br/>返回给调用者"]
    VERDICT -.->|"InspectionCompleted"| BUS(("事件总线"))

    PARAM[("方案版本<br/>参数 · ROI")] ==> PLAN
```

---

## 图 7：产线检测与热切换的消息时序（已设计，待实现）

关键点：**切换只发生在 `beginInspection()` 里**。正在进行的检测继续使用旧上线包；每次检测开始时就确定它使用的上线包和标定档案（H-04、S3、S5）。

```mermaid
sequenceDiagram
    autonumber
    actor D as 方案开发者
    participant CAM as 图像源
    participant W as 工位工作线程<br/>应用层
    participant ST as Station 聚合
    participant LD as 后台加载线程
    participant ES as ExecutionService
    participant BUS as 事件总线
    participant SUB as 订阅者<br/>数据 · 质量分析

    Note over ST: 生效上线包 = R-1

    D->>ST: deploy(R-2)
    Note over ST: DeploymentPolicy：未撤回、本平台可部署、标定有效
    ST-)BUS: ReleaseDeployed
    ST->>LD: 加载 R-2
    Note over LD: 编译执行计划、加载模型、预热<br/>期间 R-1 照常检测

    CAM->>W: 图像 A
    W->>ST: beginInspection()
    ST-->>W: 使用 R-1 + 标定 CP-1
    W->>ES: inspect(R-1, 图像 A)

    LD->>ST: markPendingReady()
    ST-)BUS: ReleaseReady

    ES-->>W: 判定
    ES-)BUS: InspectionCompleted（R-1）
    BUS-)SUB: 图像缓存、统计
    W->>ST: recordOutcome(判定)

    CAM->>W: 图像 B
    W->>ST: beginInspection()
    Note over ST: 有已就绪的待切换上线包 → 切换<br/>记录切换前的 NG 率，开启观察窗口
    ST-)BUS: HotSwapped（R-1 → R-2）
    ST-->>W: 使用 R-2 + 标定 CP-1
    W->>ES: inspect(R-2, 图像 B)
    ES-->>W: 判定
    ES-)BUS: InspectionCompleted（R-2）
    W->>ST: recordOutcome(判定)

    alt 连续 N 次 ERROR
        ST-)BUS: RolledBack（自动，回到 R-1）
    else 观察窗口内 NG 率偏离超过阈值
        ST-)BUS: QualityDeviationAlerted（只告警，由人决定）
    end
```

---

## 图 8：数据到模型的闭环（已设计，待实现）

从产线图像出发，经过复判、标注、外部训练、模型工具链，回到产线。粗线是文件或文档，虚线是领域事件。

```mermaid
flowchart TB
    CAM(["相机"]) ==>|"图像帧"| STA["BC-5 工位运行"]
    STA -->|"inspect"| EXE["BC-3 检测执行"]
    EXE -.->|"InspectionCompleted<br/>判定 · 判定依据 · 图像引用"| DAT
    EXE -.->|"InspectionCompleted"| QUA["BC-10 质量分析<br/>检测记录 · 复判"]

    subgraph DAT["BC-9 数据"]
        direction TB
        CACHE[("图像缓存<br/>全部图像，保留 72 小时")]
        SAMPLE[("样本<br/>按采集策略长期保存")]
        DS["数据集 → 标注 → 冻结数据集版本"]
        CACHE -->|"采集策略：NG 全存，OK 抽样"| SAMPLE
        SAMPLE --> DS
    end

    QUA -.->|"MisjudgmentConfirmed<br/>误报 / 漏检"| DAT
    CACHE -->|"漏检时找回图像"| SAMPLE

    DS ==>|"数据集导出"| TRAIN(["外部训练"])
    TRAIN ==>|"ONNX + 元数据<br/>契约 · 来源数据集版本"| MOD["BC-7 模型仓库"]
    MOD -->|"量化 · 转换 · 验证任务"| TCH["BC-8 模型工具链<br/>Linux Docker"]
    TCH ==>|"各平台模型制品<br/>.onnx · .ms"| MOD
    MOD -.->|"ModelVersionPublished"| REL["BC-4 上线管理"]
    REL -->|"验收检查 · 一致性检查"| EXE
    REL ==>|"上线包<br/>方案版本 + 锁定的模型版本"| STA

    classDef store fill:#fff8e1,stroke:#b8860b
    class CACHE,SAMPLE store
```

可追溯链（NFR-06）：每次**检测** → 上线包 → 方案版本 + 模型版本 → 数据集版本 → 样本 → 产生这个样本的那次检测。

---

## 修订记录

| 版本 | 日期 | 说明 |
|---|---|---|
| v0.1 | 2026-11-12 | 初稿：8 张图，覆盖模块关系、消息传递、数据流向；标注 POC-1 已实现的部分 |
