# 05 战术设计（核心上下文）

| 项 | 内容 |
|---|---|
| 版本 | v0.1（初稿，待评审） |
| 日期 | 2026-11-12 |
| 范围 | 共享内核，以及四个核心上下文：BC-2 方案编排、BC-3 检测执行、BC-4 上线管理、BC-5 工位运行 |

## 0. 约定

- **聚合之间只用 ID 引用**，不直接持有对方的对象。
- **一次操作只修改一个聚合**；跨聚合的协作通过领域事件，由应用层的事件处理器完成。
- **ID 使用全局唯一 ID**（UUID），因为方案和上线包要在多台机器之间传递（H-06）。
- **被拒绝的命令不是领域事件。** 事件风暴里标了 ❌ 的“连线被拒绝”等，在代码中是命令返回的错误（`Result::error`），不会被记录或发布。领域事件只记录**已经发生**、并且业务关心的状态变化。

---

## 1. 共享内核：视觉基础类型

BC-1、BC-2、BC-3 共用。**全部是值对象，不可变。**

| 类型 | 说明 |
|---|---|
| `Image` | 像素数据（内部共享缓冲区，复制时不拷贝像素） |
| `Region` | 算法计算出的像素集合 |
| `Roi` | 矩形 / 旋转矩形 / 圆；构造时校验宽高 > 0 |
| `Pose` | 平移 + 旋转（+ 缩放）；`Pose::apply(Roi) -> Roi` |
| `Point2d` / `Line2d` / `Circle2d` | 亚像素几何元素；`Point2d::distanceTo()` 等 |
| `Unit` | 像素、毫米、度…… |
| `Tolerance` | 标称值 + 上下偏差 |
| `PortType` | 端口类型枚举，以及 `isCompatible(from, to)` 规则 |

---

## 2. BC-2 方案编排

### 2.1 聚合一览

| 聚合 | 作用 | 为什么单独成为聚合 |
|---|---|---|
| `Recipe` | 方案及其**草稿**，所有编辑操作都在这里 | 编辑规则（无环、类型匹配……）涉及整张图 |
| `RecipeVersion` | 发布后的**不可变快照** | 被上线包引用；和草稿的生命周期完全不同；保持聚合小 |

### 2.2 `Recipe` 聚合

```
Recipe（聚合根）
  id: RecipeId, name, nextVersionNumber
  draft:
    ├─ Node（实体）× N
    │    nodeId, operatorRef(算子 ID@版本), operatorMeta(快照), params: ParamValue[],
    │    roi: RoiSpec, layout: NodeLayout(画布坐标)
    ├─ Edge（值对象）× N               from: PortRef → to: PortRef
    ├─ NamedRegion（实体）× N           regionId, name, roi: Roi
    ├─ ModelDependency（值对象）× N     slot, modelId, compatibleMajor
    ├─ CalibrationRequirement（值对象）? unit
    └─ TemplateRef（值对象）× N         匹配模板图像的内容哈希
```

`RoiSpec` 是三选一的值对象：`Absolute(Roi)` | `Relative(Roi，相对于输入的位姿)` | `Named(regionId)`。

**不变量（随时成立）**

| # | 规则 |
|---|---|
| R1 | 图中没有环 |
| R2 | 连线两端的端口存在，且端口类型兼容 |
| R3 | 每个输入端口最多一条连线 |
| R4 | 参数值满足算子快照里的参数定义（类型、范围） |
| R5 | `Named` ROI 引用的具名区域存在；被引用的具名区域不能删除 |
| R6 | 删除节点时，相连的连线一并删除 |

**发布时才检查的规则**

| # | 规则 |
|---|---|
| P1 | 所有必填输入端口都已连接 |
| P2 | 使用 `Relative` ROI 的节点，其位姿输入已连接 |
| P3 | 每个需要模型的节点，都声明了模型依赖 |
| P4 | 有节点输出毫米单位的测量值时，必须声明标定需求 |
| P5 | 至少有一个节点为判定提供判定依据 |

**命令 → 事件**

| 命令 | 成功后的事件 |
|---|---|
| `create` | `RecipeCreated` |
| `addNode` / `removeNode` / `connect` / `disconnect` / `setParam` / `setRoi` / `defineNamedRegion` / `removeNamedRegion` / `declareModelDependency` / `declareCalibrationRequirement` | 只修改草稿，**不发布领域事件**（编辑过程对其他上下文没有意义） |
| `upgradeNodeOperator`（见 D5-1） | — |
| `publish` | 生成一个 `RecipeVersion`，发布 `RecipeVersionPublished` |

> `publish` 会产生另一个聚合 `RecipeVersion`。按“一次只修改一个聚合”的原则，由应用层负责：调用 `recipe.publish()` 得到快照数据，再创建 `RecipeVersion` 并保存。

### 2.3 `RecipeVersion` 聚合

```
RecipeVersion（聚合根）
  id: RecipeVersionId（recipeId + 版本号）, contentHash
  graph: 节点、连线、参数、ROI、具名区域（草稿的完整快照，去掉画布布局）
  operatorRefs: 用到的算子 ID@版本
  modelDependencies, calibrationRequirement, templates
  status: Active | Deprecated
```

| 不变量 |
|---|
| 除 `status` 外，内容创建后不可修改 |
| `status` 只能从 Active 变为 Deprecated，不能反向 |

事件：`RecipeVersionDeprecated`。

### 2.4 其他

| 类别 | 内容 |
|---|---|
| 领域服务 | 无（规则都在聚合内） |
| 端口 | `OperatorCatalog`（来自 BC-1，添加节点时获取算子元数据快照） |
| 仓储 | `RecipeRepository`、`RecipeVersionRepository` |

---

## 3. BC-3 检测执行

### 3.1 模型一览

| 模型 | 类型 | 说明 |
|---|---|---|
| `ExecutionPlan` | 值对象 | 编译好的执行计划：节点按拓扑顺序排好；每个输入绑定到某个上游输出；算子实例、模型、标定都已解析。不持久化，可缓存 |
| `Inspection` | **聚合** | 一次检测 |

### 3.2 `Inspection` 聚合

```
Inspection（聚合根）
  id: InspectionId
  purpose: Production | Debug | Consistency        （谁发起的）
  context: stationId?, releaseId?, calibrationProfileIds, imageRefs
  state: Running → Completed | Failed | TimedOut
  ├─ NodeExecution（实体）× N   nodeId, status(Succeeded|Failed|Skipped), duration, outputs
  ├─ InspectionResult（值对象）  defects: Defect[], measurements: Measurement[], poses: Pose[]
  └─ Verdict（值对象）           OK | NG | ERROR，reasons: VerdictReason[]
```

**不变量**

| # | 规则 |
|---|---|
| I1 | 节点执行按计划顺序记录 |
| I2 | 有节点执行失败时，依赖它的下游节点为 Skipped，判定为 ERROR |
| I3 | 判定只在完成时设置一次；完成后整个聚合不可修改 |
| I4 | 判定为 NG 时，至少有一条不合格的判定依据 |

**事件**：`InspectionStarted`、`NodeExecuted`、`NodeFailed`、`InspectionCompleted`（带判定、判定依据、结果项、图像引用、上线包、标定档案）、`InspectionTimedOut`。

> `Debug` 用途的检测**不发布** `InspectionCompleted`，避免调试数据进入统计和数据集。

### 3.3 领域服务

| 服务 | 职责 |
|---|---|
| `ExecutionPlanner` | 方案版本（或草稿）+ 已解析的模型 + 标定档案 → `ExecutionPlan` |
| `Executor` | 按计划依次调用算子，把每个节点的结果记录进 `Inspection` |
| `Judge` | 汇总所有判定依据，得出 `Verdict` |

### 3.4 端口与仓储

| 类别 | 内容 |
|---|---|
| 端口 | `OperatorFactory`（BC-1）、`InferBackend`、`ImageCodec` |
| 仓储 | `InspectionRepository`（只保存 Production 用途的检测；数据量大，需要保留策略） |

---

## 4. BC-4 上线管理

### 4.1 `Release` 聚合

```
Release（聚合根）
  id: ReleaseId
  recipeVersion: RecipeVersionId
  lockedModels: { slot → ModelVersionId }
  validations: { TargetPlatform → ValidationRecord（实体） }
                  ValidationRecord: goldenSetVersion, status(Passed|Failed), report, checkedAt
```

**不变量**

| # | 规则 |
|---|---|
| L1 | 方案版本和锁定的模型版本在创建后**不可修改**（要换就创建新的上线包） |
| L2 | 每个锁定的模型版本都在方案声明的兼容范围内（主版本号相同） |
| L3 | 方案版本必须是 Active 状态 |
| L4 | 在某个平台上**可部署**，当且仅当该平台的验证记录为 Passed |

**事件**：`ReleaseCreated`、`ReleaseValidated(platform)`、`ReleaseValidationFailed(platform)`。

**领域服务** `ReleaseAssembler`：创建上线包时，从 BC-7 查询模型契约，检查 L2、L3。

### 4.2 `GoldenSet` 聚合（基准图集）

```
GoldenSet（聚合根）
  id, version, recipeId
  cases: GoldenCase（值对象）× N   imageRefs, expectedResult, calibrationProfile（用于测量换算）
  criteria: ConsistencyCriteria（值对象，由方案开发者设定，见 D5-2）
     ├─ verdictMustMatch: bool（默认 true）
     ├─ measurement: 默认规则（绝对值，或公差带的比例）+ 按测量项单独设置
     ├─ defect: countMustMatch（默认 true）、minOverlap（默认 0.9）
     └─ pose: maxTranslationPx、maxAngleDeg
```

| 不变量 |
|---|
| 期望结果必须由方案开发者确认 |
| 冻结前必须设定一致性标准（可以直接采用平台提供的默认值） |
| 冻结后不可修改；**修改一致性标准或用例 = 生成新的基准图集版本**（验证记录引用的是具体版本） |

**领域服务** `ConsistencyChecker`：把实际检测结果和期望结果按容差比较，生成报告。

### 4.3 方案包

| 模型 | 说明 |
|---|---|
| `RecipePackage`（值对象） | 清单（schemaVersion、包 ID、校验和）+ 方案版本 + 依赖声明 + 模板等资源 |
| `PackageImporter`（领域服务） | 执行 H-06 的导入规则：缺算子拒绝；缺模型制品可导入但标记不能上线；同 ID 不同内容拒绝；重复导入幂等 |
| `PackageExchange`（端口） | 导出 / 导入；先实现文件适配器 |

事件：`RecipePackageImported`。

---

## 5. BC-5 工位运行

### 5.1 `Station` 聚合

```
Station（聚合根）
  id: StationId, name
  cameras: Camera（实体）× N    cameraId, imageSourceRef, calibrationProfileId?
  state: Stopped | Running | Paused | Faulted
  activeRelease: ReleaseId?
  pendingRelease: { releaseId, ready: bool }?
  releaseHistory: ReleaseId[]（用于回滚）
  policy: cycleTime, autoRollbackAfterErrors: N
  consecutiveErrors: int
```

**不变量**

| # | 规则 |
|---|---|
| S1 | 运行中（Running）时必须有生效上线包 |
| S2 | 最多一个待切换上线包；新的部署会替换尚未切换的旧部署 |
| S3 | **只在检测开始前切换**：切换只发生在 `beginInspection()` 中（H-04） |
| S4 | 回滚的目标必须在历史中存在 |
| S5 | 每次检测开始时，确定它使用的上线包和各相机的标定档案，此后不再变化 |

**命令 → 事件**

| 命令 | 说明 | 事件 |
|---|---|---|
| `start` | 恢复上次生效的上线包（热启动） | `StationStarted`、`StationWarmStarted` |
| `deploy(release)` | 由 `DeploymentPolicy` 先检查可部署性 | `ReleaseDeployed` |
| `markPendingReady` | 应用层在后台加载、预热完成后调用 | `ReleaseReady` |
| `markPendingFailed` | 加载失败，丢弃待切换上线包，继续使用当前的 | `ReleaseLoadFailed` |
| `beginInspection()` | 如果有已就绪的待切换上线包，先切换；返回本次检测使用的上线包和标定档案 | `HotSwapped`（如果切换了） |
| `recordOutcome(verdict)` | 统计连续 ERROR 次数，达到 N 次就自动回滚 | `RolledBack(auto)` |
| `rollback()` | 手动回滚到上一个上线包 | `RolledBack(manual)` |
| `assignCalibration(cameraId, profileId)` | 重新标定后更新；从下一次检测开始生效 | `CalibrationAssigned` |
| `pause` / `resume` / `stop` | | `StationPaused` 等 |

**领域服务** `DeploymentPolicy`：检查上线包在本平台已验证通过（L4）；方案有标定需求时，相关相机都有有效标定（D4-1）。

**端口与仓储**：`StationRepository`；`ImageSource`（图像源端口）；`ResourceScheduler`（预留，H-10）。

### 5.2 一次产线检测的完整协作（应用层）

```
图像到达
 → station.beginInspection()         [Station 聚合] 必要时热切换；确定上线包和标定
 → 保存 station
 → planner.plan(...) + executor.run  [Inspection 聚合] 执行检测
 → 保存 inspection，发布 InspectionCompleted
 → 事件处理器：station.recordOutcome(verdict)  [Station 聚合] 可能触发自动回滚
 → 事件处理器：采集策略（BC-9）、统计（BC-10）
```

每一步只修改一个聚合。

---

## 6. 🟥 决策点

### 已决定

| # | 问题 | 决定 | 日期 |
|---|---|---|---|
| D5-2（H-03） | “结果一致”怎么定义？ | **由用户设定标准**：一致性标准 `ConsistencyCriteria` 是基准图集上的值对象，由方案开发者配置（判定是否必须相同；测量值容差，可按测量项单独设置；缺陷数量和位置重叠度；位姿阈值）。平台只提供默认值。修改标准即生成新的基准图集版本 | 2026-11-12 |

### 待确认

| # | 问题 | 建议 |
|---|---|---|
| **D5-1**（H-01） | **算子升级后，已发布的方案怎么办？** | 算子也用**主版本号表示兼容性**。节点保存 `算子 ID@版本` 和添加时的元数据快照；方案版本记录用到的算子版本。兼容的小版本升级自动生效；不兼容的大版本升级需要方案开发者执行“升级节点”命令（迁移参数），然后发布新的方案版本。导入方案包和部署时，检查本平台有没有对应主版本的算子 |
| D5-3 | `Recipe`（草稿）和 `RecipeVersion`（快照）分成两个聚合 | 分开：生命周期不同，聚合要小 |
| D5-4 | 一个工位同一时刻只运行一个检测吗？ | **是**，先按顺序执行。流水线并行（上一张还没检测完，下一张就开始）放到以后 |
