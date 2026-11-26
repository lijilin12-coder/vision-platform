# Vision Platform

可编排的工业外观缺陷检测与测量平台。用户通过拖拽算子组合检测方案，完成缺陷检测、尺寸测量和定位；方案与平台无关，支持数据闭环、模型工具链，以及产线不停机的热更新。

- **部署平台**：鸿蒙电脑（主要）、Windows（第二平台）；Linux 保持可编译
- **开发平台**：Windows 和 Linux，由自动构建保证互不影响（不考虑 macOS；之后适配鸿蒙）
- **核心**：C++17，领域驱动设计 + 六边形架构，各平台共用
- **界面**：Web（Vue 3），鸿蒙端为原生 ArkUI 外壳加 ArkWeb
- **模型工具链**：独立的 Linux 服务（Python + Docker）

> 当前阶段：后端到了 MVP，界面可以演示一整圈：搭方案 → 验收 → 上线包文件 → 产线运行 → 检测记录 → 复判 → 用复判的图像验收下一版方案。
> 做完了什么、还差什么见 [docs/07-backend-mvp.md](docs/07-backend-mvp.md)；想动手试，照着 [docs/dev/web-demo.md](docs/dev/web-demo.md) 走一遍。

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

文档很多，从 **[docs/README.md（文档地图）](docs/README.md)** 进去：它按主题列出该读哪一份，并且分清了现在的设计、历史记录和教程。

## 目录结构

```
vision-platform/
├── core/          # kernel（共享内核）、common、contexts/（每个限界上下文一个目录）
├── operators/     # 算子实现
├── adapters/      # 被驱动适配器：存储、推理、图像、包文件、事件总线……
├── apps/          # 三个程序：cli、runtime（HTTP 服务 + 界面）、line（产线运行平台）
├── web/           # 界面（Vue 3）
├── services/      # 独立运行的服务：训练服务（Python）
├── tests/         # C++ 测试
├── samples/       # 示例图像、方案、工作流
└── docs/
```

每个目录管什么、有哪些必须遵守的规则，见 [CLAUDE.md](CLAUDE.md)（写给 AI 助手的，人读也合适）。
