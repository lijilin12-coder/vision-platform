# Vision Platform

可编排的工业外观缺陷检测与测量平台。用户通过拖拽算子组合检测流程，完成外观缺陷检测和尺寸测量。

- **运行平台**：macOS（Apple Silicon）、HarmonyOS 电脑（2in1）
- **核心**：C++17 检测引擎（vision-core），两端共用
- **界面**：Web 编辑器（Vue 3），鸿蒙端为原生 ArkUI 外壳加 ArkWeb 嵌入编辑器

> 当前阶段：**需求与设计**。先完成文档评审，再进入 POC 开发。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/01-requirements.md](docs/01-requirements.md) | 需求规格：角色、功能、非功能、范围 |
| [docs/02-architecture.md](docs/02-architecture.md) | 架构设计与选型决策（ADR） |
| [docs/03-tech-research.md](docs/03-tech-research.md) | 技术调研：鸿蒙电脑、OpenCV 移植、ArkWeb 桥接、推理等 |
| [docs/04-poc-plan.md](docs/04-poc-plan.md) | POC 计划与通过标准 |
| [docs/05-roadmap.md](docs/05-roadmap.md) | 里程碑、分工与比赛时间线 |
| [docs/06-open-questions.md](docs/06-open-questions.md) | 待决问题清单 |

## 计划中的目录结构

```
vision-platform/
├── core/        # vision-core：数据类型、算子、DAG、方案 JSON（C++17）
├── server/      # Mac 端本地 HTTP/WebSocket 服务（C++）
├── web/         # Web 编辑器（Vue 3 + TypeScript）
├── harmony/     # 鸿蒙电脑应用（ArkTS + NAPI）
├── models/      # ONNX 模型与转换脚本
├── samples/     # 示例图像与示例方案
└── docs/        # 设计文档
```
