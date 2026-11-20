# Vision Platform

可编排的工业外观缺陷检测与测量平台。用户通过拖拽算子组合检测方案，完成缺陷检测、尺寸测量和定位；方案与平台无关，支持数据闭环、模型工具链，以及产线不停机的热更新。

- **部署平台**：鸿蒙电脑（主要）、Windows（第二平台）；Linux 保持可编译
- **开发平台**：macOS 和 Windows 都是开发机，由自动构建保证互不影响
- **核心**：C++17，领域驱动设计 + 六边形架构，各平台共用
- **界面**：Web（Vue 3），鸿蒙端为原生 ArkUI 外壳加 ArkWeb
- **模型工具链**：独立的 Linux 服务（Python + Docker）

> 当前阶段：**POC**。已完成：POC-1（核心骨架，[报告](docs/poc/poc-1-report.md)）、POC-9（热切换，[报告](docs/poc/poc-9-report.md)）、POC-11（方案助手的最小闭环，[报告](docs/poc/poc-11-report.md)）、POC-12（大模型顾问，[报告](docs/poc/poc-12-report.md)，鸿蒙部分未验证）。

## 快速开始

```bash
cmake --preset default && cmake --build --preset default && ctest --preset default   # Windows 上把 default 换成 windows
./build/apps/vision-cli sample-images samples/images
RID=$(./build/apps/vision-cli recipe build samples/recipes/bright-spot.draft.json)
VID=$(./build/apps/vision-cli recipe publish $RID)
./build/apps/vision-cli inspect $VID samples/images
```

依赖和更多命令见 [docs/dev/build.md](docs/dev/build.md)；新增算子见 [docs/dev/how-to-add-operator.md](docs/dev/how-to-add-operator.md)。

## 文档

### 领域设计（DDD）

| 文档 | 内容 |
|---|---|
| [docs/ddd/00-vision.md](docs/ddd/00-vision.md) | 领域愿景 |
| [docs/ddd/01-event-storming.md](docs/ddd/01-event-storming.md) | 事件风暴、热点问题与决定 |
| [docs/ddd/02-glossary.md](docs/ddd/02-glossary.md) | 统一语言（术语表） |
| [docs/ddd/03-subdomains.md](docs/ddd/03-subdomains.md) | 子域划分 |
| [docs/ddd/04-context-map.md](docs/ddd/04-context-map.md) | 限界上下文与上下文映射 |
| [docs/ddd/05-tactical-design.md](docs/ddd/05-tactical-design.md) | 核心上下文的战术设计 |
| [docs/ddd/06-scenario-walkthrough.md](docs/ddd/06-scenario-walkthrough.md) | 关键场景走查与发现的问题 |

### 工程文档

| 文档 | 内容 |
|---|---|
| [docs/02-architecture.md](docs/02-architecture.md) | 技术架构（v0.2，按 DDD 重写） |
| [docs/07-architecture-diagrams.md](docs/07-architecture-diagrams.md) | 架构图：模块关系、消息传递、数据流向 |
| [docs/03-tech-research.md](docs/03-tech-research.md) | 技术调研 |
| [docs/01-requirements.md](docs/01-requirements.md) | 需求规格（v0.2，按限界上下文组织） |
| [docs/04-poc-plan.md](docs/04-poc-plan.md) | POC 计划（v0.2） |
| [docs/05-roadmap.md](docs/05-roadmap.md) | 里程碑与分工 |
| [docs/06-open-questions.md](docs/06-open-questions.md) | 待决问题 |
| [docs/poc/poc-1-report.md](docs/poc/poc-1-report.md) | POC-1 结论报告：核心骨架 |
| [docs/poc/poc-9-report.md](docs/poc/poc-9-report.md) | POC-9 结论报告：热切换 |
| [docs/poc/poc-11-report.md](docs/poc/poc-11-report.md) | POC-11 结论报告：方案助手的最小闭环（不用大模型） |
| [docs/poc/poc-12-report.md](docs/poc/poc-12-report.md) | POC-12 结论报告：大模型顾问（DeepSeek 实测） |
| [docs/poc/hardening-report.md](docs/poc/hardening-report.md) | 方案助手的加固阶段：解决“达标的参数贴着边界” |
| [docs/dev/build.md](docs/dev/build.md) | 构建与运行 |
| [docs/dev/how-to-add-operator.md](docs/dev/how-to-add-operator.md) | 如何新增一个算子 |
| [docs/dev/logging-and-crash-dumps.md](docs/dev/logging-and-crash-dumps.md) | 日志与崩溃转储：文件在哪里、怎么写日志、怎么分析崩溃 |
| [docs/tutorial/cpp/](docs/tutorial/cpp/README.md) | 现代工程 C++ 教程（从竞赛写法到工程写法） |
| [docs/tutorial/opencv/](docs/tutorial/opencv/README.md) | OpenCV 与机器视觉基础教程（以“带旋转角度的模板匹配”大作业结束） |
| [docs/tutorial/inference/](docs/tutorial/inference/README.md) | 推理引擎与大模型推理优化教程（算子、计算图、量化、NPU 后端；KV Cache、Continuous Batching、PagedAttention 等） |

## 目录结构（计划）

```
vision-platform/
├── core/          # kernel（共享内核）、common、contexts/（每个限界上下文一个目录）
├── operators/     # 算子实现
├── adapters/      # 被驱动适配器：持久化、推理、图像、方案包、事件总线……
├── apps/          # runtime（Linux/Windows/Mac）、harmony、cli
├── web/           # Web 界面
├── toolchain/     # 模型工具链服务
├── samples/       # 示例图像、方案、基准图集
└── docs/
```
