# 给 Claude 的项目说明

新会话先读这一份，再按任务去读“文档地图”（`docs/README.md`）里对应的那一两份设计文档。**不要通读 `docs/`**——大部分是历史记录和教程。

## 这是什么

可编排的工业外观缺陷检测与测量平台：把算子连成检测方案，验收后带到产线上运行。

这是一个**学习项目**：使用者是一位大一学生（会 C++），她的哥哥是架构师和导师。所以：

- **和用户说中文**；代码注释、文档、提交信息、PR 也都用中文
- 文档先行：一个功能先有设计文档（`docs/ddd/NN-*.md`），再有代码
- 文字要让大一学生读得懂：不用黑话，解释“为什么”
- 术语以 `docs/ddd/02-glossary.md` 为准。特别注意：**“算子”是 Operator，产线上的“操作员”是 LineOperator**，代码里不要混用

平台只考虑 **Windows 和 Linux**（都必须能编译、通过全部测试），以后适配鸿蒙。不考虑 macOS。

## 目录

```
core/kernel/            共享内核：Image、Value、算子接口、结果项、模型接口
core/common/            Result、领域事件、日志、路径（UTF-8）、时间
core/contexts/<名字>/   限界上下文，每个都是 domain/（规则）+ application/（服务、端口、DTO）
operators/              18 个算子的实现（用 OpenCV）
adapters/               被驱动适配器：存储、内容仓库、推理、图像源、包文件、大模型……
apps/common/            组装根（把适配器和服务连起来）、三个程序共用的命令
apps/cli|runtime|line/  三个程序
web/                    界面（Vue 3 + TypeScript）
services/training/      训练服务（Python，独立进程，只通过 HTTP 和平台说话）
tests/                  C++ 测试（GoogleTest）
samples/ tools/ cmake/  示例、脚本、构建辅助
```

限界上下文（`core/contexts/`）：

| 目录 | 管什么 | 设计文档 |
|---|---|---|
| `catalog` | 算子目录 | ddd/05 |
| `authoring` | 方案编排：草稿、节点、连线、参数、ROI、匹配模板、发布成方案版本 | ddd/05、09 |
| `execution` | 检测执行：把方案版本编译成执行计划、运行、得出判定和结果项 | ddd/05、10 |
| `release` | 上线管理：上线包、基准图集、验收检查、方案包和上线包文件 | ddd/12、15、16 |
| `station` | 工位运行：部署、热切换、回滚、图像源 | ddd/05、11 |
| `quality` | 质量分析：检测记录、复判、检测图像的保留 | ddd/14、17、18 |
| `data` | 样本、数据集版本（规则 N1：基准和训练不混用） | ddd/08 |
| `models` | 模型仓库 | ddd/08 |
| `training` | 训练任务 | ddd/08、poc-13 |
| `assistant` | 方案助手：按工作流搭方案、自动调参 | ddd/07 |

三个程序：

| 程序 | 平台 | 是什么 |
|---|---|---|
| `vision-cli` | 方案构建平台 | 命令行，什么都能做 |
| `vision-runtime` | 方案构建平台 | HTTP 服务 + 界面（`web/dist`） |
| `vision-line` | 产线运行平台 | 只有：导入、上线包、工位、`serve`。没有方案编排、训练、数据 |

`apps/common/line_platform.h` 组装产线那一侧；`platform.h` 在它之上加方案构建那一侧。

## 必须遵守的架构规则

- **依赖方向由 CMake 强制**：上下文用 `vision_domain_library` / `vision_application_library` 定义。应用层的公开头文件（`api.h`）不能包含领域层的头文件；上下文之间只能通过应用层的接口或端口打交道。违反了会编译不过，不要绕
- **领域规则写在领域层，并且有编号**（L4、Q1、R3、M2……）。新规则：起编号、写进设计文档、在领域层实现、写一个以编号开头的测试
- 上下文之间的翻译写在组装根（`apps/common`），不要让一个上下文认识另一个上下文的类型
- 不可变的东西不改：方案版本、检测记录、复判都是只追加
- 出错用 `Result<T>` / `Status` 返回（`code` + 给人看的中文 `message`），不抛异常。HTTP 接口把服务的 `code`、`message` 原样交出去
- 路径一律 UTF-8：用 `pathFromUtf8` / `pathToUtf8`，测试里故意用中文目录名

## 构建和测试

```bash
cmake --preset default && cmake --build --preset default && ctest --preset default
```

Windows 上把 `default` 换成 `windows`（详见 `docs/dev/build.md`）。只跑一部分：`ctest --test-dir build -R <名字>`，或者直接运行 `build/tests/<某>_tests --gtest_filter=…`。

界面（在 `web/` 里）：

```bash
npm install && npm run build
```

```bash
npm test
```

```bash
npm run e2e
```

`npm run e2e` 会启动真的 `vision-runtime` 和真的浏览器，要先编译好 C++ 和界面。

改了并发或内存相关的代码，另外用 ASan 和 TSan 各跑一遍（Linux）：

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" && cmake --build build-asan && ctest --test-dir build-asan
```

```bash
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" && cmake --build build-tsan && TSAN_OPTIONS="suppressions=$PWD/tests/support/tsan.supp" ctest --test-dir build-tsan
```

## 做一个功能的流程

1. 从最新的 `main` 开分支
2. 写（或更新）设计文档；有和已有设计不一致的地方，在文档里单列一节“请确认的决定”，并在回复里告诉用户
3. 领域层 → 应用层 → 适配器 → 组装 → 命令行 / HTTP → 界面，每一层带测试
4. 更新受影响的文档：`docs/dev/http-api.md`、`docs/dev/web-demo.md`、命令行的帮助文字、`docs/07-backend-mvp.md`
5. 全部测试通过后提交、推送、开 PR。PR 说明里写：做了什么、对已有东西的影响、请确认的决定、怎么验证的
6. CI 在 Windows 和 Linux 上都要通过。**合并要用户明确说可以**

用户确认了“请确认的决定”之后，把文档里那一节改成“定下来的决定（日期）”。

## 容易踩的坑

- **算子是 OBJECT 库**（靠全局对象的构造函数注册自己）：每个可执行文件和测试都要**直接**链接 `vision_operators`，否则算子目录是空的
- **用到推理的目标**要调用 `vision_copy_onnxruntime(<目标>)`（Windows 上要把 DLL 拷到旁边）
- **规则 L4：只有验收通过的上线包才能部署。** 不是在测验收的测试，建好平台后要 `stations.allowUnacceptedReleases(true)`；命令行对应 `--allow-unaccepted`
- **同样内容的图像是同一个样本**，不能在一个基准图集里出现两次：测试里造图像要让它们各不相同
- **Windows 上文件开着就删不掉**：测试里读完文件要先关上（放进一个作用域），再删目录
- **CI 的 PowerShell 步骤**里，如果最后一条命令是“预期会失败”的，后面要加 `$global:LASTEXITCODE = 0`
- **界面的端到端测试**：点完之后要等页面变化（`waitForFunction` / `waitForSelector`），不要读完就断言；切换页面会重建组件，表单会清空
- `web/` 里只有 `HttpPlatformClient.ts` 可以调用 `fetch`；组件只做显示，状态和逻辑在 `editor.ts`、`stations.ts`、`releases.ts` 里（各有单元测试）
- 长时间运行的程序退出前，工位的后台加载要先结束（`LinePlatform` 的析构函数里有，新增成员时注意声明顺序 = 构造顺序）
- 训练服务的图像预处理必须和 C++ 算子完全一致（OpenCV `cvtColor` → `resize(INTER_AREA)` → ÷255），否则两边算出来的分数对不上

## 现在的状态

`docs/07-backend-mvp.md` §5 是“还差什么”的清单，以它为准。
