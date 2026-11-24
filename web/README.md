# 界面（Web）

方案编辑器：在浏览器里搭方案、改参数、选一幅图像运行、看每个节点的中间结果。

技术：Vue 3 + TypeScript + Vite，画布用 Vue Flow。

## 怎么运行

需要 **Node.js 20 以上**（https://nodejs.org ，装 LTS 版）。只改 C++ 的话不需要它。

```bash
cd web
npm install          # 第一次，或者 package.json 变了之后
npm run build        # 生成 web/dist
```

国内下载慢的话，先换镜像源：`npm config set registry https://registry.npmmirror.com`

然后回到仓库根目录启动后端，它会顺带把界面交给浏览器：

```bash
./build/apps/vision-runtime --data ./vision-data
```

浏览器打开 http://127.0.0.1:8750/

## 改界面的时候

开两个终端：一个跑后端（上面那条命令），一个在 `web` 目录里跑

```bash
npm run dev
```

打开它提示的地址（http://localhost:5173）。改了代码，浏览器里马上能看到，不用重新编译。`/api` 开头的请求会自动转给后端。

## 代码怎么读

从外到里：

| 文件 | 是什么 |
|---|---|
| `src/platform/PlatformClient.ts` | **界面使用后端的唯一入口**（一个接口）。先读它：界面能让后端做的事都列在这里 |
| `src/platform/HttpPlatformClient.ts` | 这个接口的 HTTP 实现。**整个界面只有这一个文件发 HTTP 请求** |
| `src/platform/types.ts` | 和后端交换的数据长什么样 |
| `src/editor.ts` | 编辑器的状态和操作：当前的方案、选中的节点、运行的结果；加节点、连线、改参数、自动重新运行 |
| `src/stations.ts` | 工位页面的状态和操作：工位列表、部署、开始和停止、隔半秒取一次新的检测记录 |
| `src/components/*.vue` | 看得见的部分：算子面板、画布、节点、参数面板、预览。它们只做显示，把用户的动作交给 `editor` |
| `src/main.ts` | 把上面这些组装起来 |

三条贯穿始终的原则：

1. **规则在后端。** 界面不判断“这条线能不能连”“这个参数合不合法”。每个操作都是“请后端改 → 用后端回复的最新方案替换本地的”；
   后端拒绝时，把它给的原因原样显示出来。（拖线时类型对不上的端口放不下去，那只是给人的提示，不是规则。）
2. **界面不认识具体的算子。** 算子面板、节点上的端口、参数面板里的输入框，全部按后端给的算子定义自动生成。
   后端新增一个算子（一个 `.cpp` 文件），界面一行都不用改。
3. **界面的其余部分不知道后端是通过 HTTP 访问的。** 以后在鸿蒙上，界面跑在 ArkWeb 里，换一个 `PlatformClient` 的实现就行。

## 测试

```bash
npm test             # 单元测试：编辑器的状态和操作，对着一个假的后端。不需要启动后端，也不需要浏览器
npm run e2e          # 端到端：真的浏览器 + 真的后端。要先编译好后端和界面（npm run build）
```

`npm run e2e` 需要 Playwright：`npm install --no-save playwright`，然后 `npx playwright install chromium`。

## 现在还没有的

- 用鼠标框匹配模板（现在只能用命令行框）
- 撤销 / 重做；改方案的名称；删除方案
- 训练、模型仓库、方案助手、方案包的页面
- 节点在画布上的位置只记在这个浏览器里，不属于方案：换一台电脑打开，会重新自动排
