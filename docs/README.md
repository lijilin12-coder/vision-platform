# 文档地图

文档很多。这一页说的是：**哪些是现在的设计（要读）、哪些是历史记录（一般不用读）、哪些是教程（和代码无关）。**

做一个功能之前：读[术语表](ddd/02-glossary.md)里相关的词，再读下面“按主题找”里对应的那一两份。

## 按主题找

| 要做的事和什么有关 | 读这些 |
|---|---|
| 方案、节点、连线、参数、ROI、发布 | [ddd/05](ddd/05-tactical-design.md) §2、[dev/operators](dev/operators.md) |
| 匹配模板、定位、按位姿对齐 | [ddd/09](ddd/09-match-template-and-locating.md) |
| 检测怎么执行、判定、结果项（缺陷、测量值、位姿） | [ddd/05](ddd/05-tactical-design.md) §3、[ddd/10](ddd/10-result-items.md) |
| 上线包、验收检查、基准图集 | [ddd/15](ddd/15-acceptance-check.md) |
| 把方案带到另一台机器：方案包、上线包文件 | [ddd/12](ddd/12-recipe-package.md)、[ddd/16](ddd/16-release-bundle.md) |
| 工位、部署、热切换、回滚、图像源 | [ddd/05](ddd/05-tactical-design.md) §5、[ddd/11](ddd/11-image-source.md)、[poc-9 报告](poc/poc-9-report.md) |
| 两个平台、三个程序、产线上怎么部署 | [ddd/13](ddd/13-two-programs.md)、[ddd/08](ddd/08-platforms-and-training.md) |
| 检测记录、复判、检测图像留多久 | [ddd/14](ddd/14-inspection-records.md)、[ddd/17](ddd/17-review.md)、[ddd/18](ddd/18-image-cache.md) |
| 样本、数据集、模型仓库、训练 | [ddd/08](ddd/08-platforms-and-training.md)、[poc-13 报告](poc/poc-13-report.md)、[dev/training-demo](dev/training-demo.md) |
| 方案助手（自动搭方案、调参、大模型顾问） | [ddd/07](ddd/07-recipe-assistant.md) |
| HTTP 接口 | [dev/http-api](dev/http-api.md) |
| 界面 | [web/README](../web/README.md)、[dev/web-demo](dev/web-demo.md) |
| 新增算子 | [dev/how-to-add-operator](dev/how-to-add-operator.md) |
| 编译、依赖 | [dev/build](dev/build.md) |
| 日志、崩溃转储 | [dev/logging-and-crash-dumps](dev/logging-and-crash-dumps.md) |

## 现在的设计

**总览**

| 文档 | 内容 | 状态 |
|---|---|---|
| [07-backend-mvp](07-backend-mvp.md) | 做完了什么、还差什么、各处“请确认的决定”的汇总 | **最新，先看这个** |
| [ddd/02-glossary](ddd/02-glossary.md) | 术语表。代码里的名字以它为准 | 现行 |
| [ddd/04-context-map](ddd/04-context-map.md) | 有哪些限界上下文、它们之间怎么打交道 | 现行 |
| [ddd/05-tactical-design](ddd/05-tactical-design.md) | 各上下文的聚合、不变量、事件 | 现行。它是**目标设计**：有的部分还没实现，实现到哪一步看下面各篇 |
| [01-requirements](01-requirements.md) | 需求规格（需求编号 AUT-xx、REL-xx……的出处） | 现行 |

**一个功能一篇**（都写着“已实现”和日期；每篇末尾有“定下来的决定”和“还没有的”）

| 文档 | 功能 |
|---|---|
| [ddd/07](ddd/07-recipe-assistant.md) | 方案助手 |
| [ddd/08](ddd/08-platforms-and-training.md) | 平台划分、训练与数据的划分 |
| [ddd/09](ddd/09-match-template-and-locating.md) | 匹配模板与定位 |
| [ddd/10](ddd/10-result-items.md) | 结果项 |
| [ddd/11](ddd/11-image-source.md) | 图像源 |
| [ddd/12](ddd/12-recipe-package.md) | 方案包 |
| [ddd/13](ddd/13-two-programs.md) | 两个程序 |
| [ddd/14](ddd/14-inspection-records.md) | 检测记录 |
| [ddd/15](ddd/15-acceptance-check.md) | 验收检查 |
| [ddd/16](ddd/16-release-bundle.md) | 上线包文件 |
| [ddd/17](ddd/17-review.md) | 复判 |
| [ddd/18](ddd/18-image-cache.md) | 短期图像缓存 |

**怎么用**（`dev/`）：[build](dev/build.md)、[operators](dev/operators.md)、[http-api](dev/http-api.md)、[web-demo](dev/web-demo.md)、[training-demo](dev/training-demo.md)、[how-to-add-operator](dev/how-to-add-operator.md)、[logging-and-crash-dumps](dev/logging-and-crash-dumps.md)

## 历史记录（了解“当时为什么这么定”时才读）

| 文档 | 是什么 |
|---|---|
| [ddd/00-vision](ddd/00-vision.md)、[ddd/01-event-storming](ddd/01-event-storming.md)、[ddd/03-subdomains](ddd/03-subdomains.md)、[ddd/06-scenario-walkthrough](ddd/06-scenario-walkthrough.md) | 最初的领域分析：愿景、事件风暴（规则 P1–P13、热点 H-xx 的出处）、子域、场景走查（发现 F-1–F-7 的出处） |
| [02-architecture](02-architecture.md)、[07-architecture-diagrams](07-architecture-diagrams.md) | 早期的技术架构和架构图。大方向没变，细节以代码和上面的各篇为准 |
| [03-tech-research](03-tech-research.md)、[04-poc-plan](04-poc-plan.md)、[05-roadmap](05-roadmap.md)、[06-open-questions](06-open-questions.md) | 技术调研、POC 计划、路线图、待决问题 |
| [poc/](poc/) 下的各份报告 | 每个 POC 做完时的结论和实测数据。其中 [poc-9](poc/poc-9-report.md)（热切换）、[poc-13](poc/poc-13-report.md)（训练闭环，含 MVTec 实测）仍然是那两块最详细的说明 |

## 教程（和平台的代码无关）

[tutorial/cpp](tutorial/cpp/README.md)（现代工程 C++）、[tutorial/opencv](tutorial/opencv/README.md)（OpenCV 与机器视觉基础）、[tutorial/inference](tutorial/inference/README.md)（推理引擎与大模型推理优化）。
这三套是给学习用的，篇幅很大（占 `docs/` 的大部分）。做平台的功能时不用读。

## 编号是什么意思

设计文档和代码注释里到处是带编号的规则，测试的名字也以它们开头：

| 前缀 | 是什么 | 在哪里定义 |
|---|---|---|
| AUT、EXE、REL、STA、QUA、AST…-数字 | 需求 | [01-requirements](01-requirements.md) |
| H-数字、P数字 | 事件风暴里的热点问题、业务规则 | [ddd/01](ddd/01-event-storming.md) |
| F-数字 | 场景走查发现的问题 | [ddd/06](ddd/06-scenario-walkthrough.md) |
| D数字-数字 | 设计决策点 | [ddd/04](ddd/04-context-map.md)、[ddd/05](ddd/05-tactical-design.md) |
| I、P（方案）、S、L、N | 检测、方案、工位、上线包、数据的不变量 | [ddd/05](ddd/05-tactical-design.md)、[ddd/08](ddd/08-platforms-and-training.md) |
| K、A、B | 方案包导入、验收、上线包文件的规则 | [ddd/12](ddd/12-recipe-package.md)、[ddd/15](ddd/15-acceptance-check.md)、[ddd/16](ddd/16-release-bundle.md) |
| Q、R、M | 检测记录、复判、图像保留的规则 | [ddd/14](ddd/14-inspection-records.md)、[ddd/17](ddd/17-review.md)、[ddd/18](ddd/18-image-cache.md) |
