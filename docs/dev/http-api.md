# vision-runtime 的 HTTP 接口

`vision-runtime` 把平台包成一个一直开着的本地服务。它和命令行 `vision-cli` 用的是同一批应用服务、同一个数据目录，所以两边能互相看到对方的方案。

```bash
./build/apps/vision-runtime --data ./vision-data --port 8750
```

```bash
curl http://127.0.0.1:8750/api/health
```

> **没有登录和权限。** 默认只监听本机（`127.0.0.1`）。不要用 `--host` 让它监听不可信的网络。

`web/dist` 存在的话（界面编译好了，见 [web/README.md](../../web/README.md)），服务同时把界面交给浏览器：打开 http://127.0.0.1:8750/ 就是方案编辑器。

有方案编排、调试运行、上线包、工位的接口；训练、方案助手、方案包的接口以后再加。

## 1. 约定

- 请求和回复都是 JSON（UTF-8）。**发请求时写上 `Content-Type: application/json`**；上传图像写 `image/png` 之类。
  不写的话 `curl -d` 默认按“表单”发送，HTTP 库对表单有一个 8 KB 的上限，稍大一点的请求会得到 413。
- 改方案的操作成功后，回复的是**改完之后的整个方案**，不用再问一次。
- 出错时回复 `{"code": "…", "message": "…"}`。`code` 和 `message` 就是应用服务给的，和命令行里看到的一样。

| 状态码 | 含义 |
|---|---|
| 200、201 | 成功（201：新建了东西） |
| 400 | 请求本身看不懂：不是 JSON、缺字段、字段类型不对。`code` 是 `api.bad_request` |
| 404 | 找不到：方案、节点、图像、接口 |
| 413 | 请求体太大（上限 64 MB） |
| 422 | 请求看得懂，但**规则不允许**：类型对不上的连线、超出范围的参数、缺判定就发布…… |
| 500 | 服务内部出错。详情在 `<数据目录>/logs/vision-runtime.log` |

## 2. 接口

### 算子

| 请求 | 说明 |
|---|---|
| `GET /api/operators` | 全部算子。每个算子有 `id`、`displayName`、`category`、`description`、`inputs`、`outputs`、`params` |

参数定义里有 `name`、`type`（`number` / `bool` / `text` / `choice` / `template` / `model`）、`default`、`label`，
数字还有 `min`、`max`，选项还有 `choices`——足够自动生成一个参数面板。

### 方案

| 请求 | 请求体 | 说明 |
|---|---|---|
| `GET /api/recipes` | | 列出方案：`id`、`name` |
| `POST /api/recipes` | `{"name": "亮斑检测"}` | 新建 |
| `GET /api/recipes/{id}` | | 取草稿：`nodes`、`edges`、`templates` |
| `POST /api/recipes/{id}/nodes` | `{"operatorId": "vision.threshold"}` | 加节点。回复里多一个 `addedNodeId` |
| `DELETE /api/recipes/{id}/nodes/{node}` | | 删节点（它的连线一起删掉） |
| `POST /api/recipes/{id}/edges` | `{"from": "n1.image", "to": "n2.image"}` | 连线 |
| `DELETE /api/recipes/{id}/edges` | 同上 | 删连线 |
| `PUT /api/recipes/{id}/nodes/{node}/params/{name}` | `{"value": 128}` | 改参数。值可以是数字、布尔值、文本 |
| `PUT /api/recipes/{id}/nodes/{node}/roi` | `{"x": 0, "y": 0, "width": 100, "height": 80}` | 设置 ROI |
| `DELETE /api/recipes/{id}/nodes/{node}/roi` | | 清除 ROI |
| `POST /api/recipes/{id}/publish` | | 发布。回复 `{"versionId": "…@v1"}` |

### 图像和调试运行

| 请求 | 说明 |
|---|---|
| `POST /api/images` | 请求体是图像文件的内容（PNG、JPEG、BMP）。回复 `imageId`（内容的哈希）、`width`、`height`、`channels`。同一幅图像传几次，`imageId` 都一样 |
| `GET /api/images/{imageId}` | 取回上传过的图像（PNG） |
| `GET /api/models` | 模型仓库里的模型版本：`id`、`status`、`kind`（界面里“模型”参数的下拉列表用） |
| `POST /api/recipes/{id}/debug-runs` | 请求体 `{"imageId": "…"}`。用草稿当前的样子运行一次 |
| `GET /api/debug-runs/{run}/outputs/{node}/{port}` | 取一个图像或区域输出，PNG。区域是黑白图（选中的像素是白的） |

### 上线包和工位

| 请求 | 请求体 | 说明 |
|---|---|---|
| `GET /api/releases` | | 上线包：`id`、`recipeVersionId`、`recipeName`、`withdrawn`、`acceptanceState`（`none` 没验收 / `passed` / `failed`）、`acceptance`（最近一次验收的记录，没有时是 `null`） |
| `POST /api/releases` | `{"recipeVersionId": "…@v1"}` | 用一个已发布的方案版本做成上线包。方案版本加载不了（例如用了没发布的模型）时被拒绝 |
| `POST /api/releases/{id}/acceptance` | `{"goldenSetId": "…"}` | **验收检查**（[说明](../ddd/15-acceptance-check.md)）：在基准图集上跑一遍，对照真实判定。回复是验收记录：`passed`、`criteria`、`total`、`misses`（漏检）、`falseAlarms` 和 `falseAlarmRate`（误报）、`errors`、`wrongCases`（判错的用例）。**没通过也是 200**（`passed` 为 `false`）；做不下去（图像读不出来等）才是 4xx。用例多的时候要等一会儿 |
| `POST /api/releases/{id}/export` | `{"path": "某个文件.vrel"}` | 导出**上线包文件**（[说明](../ddd/16-release-bundle.md)），写在后端那台机器上。只有验收通过、没撤回的上线包才能导出（否则 422） |
| `GET /api/golden-sets` | | 基准图集：`id`、`name`、`cases`、`truthOk`、`truthNg`、`criteria` |
| `POST /api/golden-sets` | `{"name", "okDirectory", "ngDirectory", "maxMisses", "maxFalseAlarmRate", "maxErrors"}` | 用后端那台机器上的两个目录做一个基准图集。目录可以只给一个；三个标准不给就是 0；误报率是小数（0.02 = 2%） |
| `GET /api/stations` | | 所有工位的状态（不带检测记录） |
| `POST /api/stations` | `{"name": "一号工位"}` | 新建 |
| `GET /api/stations/{id}?after=N` | | 一个工位的状态，加上序号大于 N 的检测记录。界面隔半秒问一次，每次只取新的 |
| `PUT /api/stations/{id}/image-source` | `{"kind": "folder", "location": "某个目录"}` | 绑定图像源。只能在工位停止时做 |
| `POST /api/stations/{id}/deploy` | `{"releaseId": "…"}` | 部署：后台加载，下一次检测开始前生效。只有验收通过的上线包才能部署：没验收的、没通过的、已撤回的都被拒绝（422）。调试时可以用 `--allow-unaccepted` 启动服务，放过“没验收”的 |
| `POST /api/stations/{id}/rollback` | | 回滚到上一个上线包 |
| `POST /api/stations/{id}/start`、`/stop` | | 开始 / 停止持续运行：工位从自己的图像源取图、检测 |
| `GET /api/stations/{id}/frame` | | 最近检测的那一帧图像（PNG） |
| `GET /api/stations/{id}/records` | | **保存下来的**检测记录，从新到旧。`?verdict=NG` 只要这种判定；`?review=unreviewed` 还没复判的、`?review=misjudged` 复判确认判错了的；`?before=<记录的 id>` 翻页，只要更早的；`?limit=N`（默认 50，最多 500） |
| `GET /api/stations/{id}/records/{记录的 id}/image` | | 这条记录当时的图像（PNG）。当时没留、已经过期清掉了：404，原因在 `message` 里 |
| `POST /api/stations/{id}/records/{记录的 id}/review` | `{"truth": "OK" 或 "NG", "note": "…", "reviewer": "…"}` | **复判**（[说明](../ddd/17-review.md)）：人确认这一次实际是合格还是不合格。后两项可以不给。可以重复，以最近一次为准。回复是那条记录，`review` 里是结论 |
| `POST /api/stations/{id}/export-reviewed` | `{"directory": "…", "misjudgedOnly": false}` | 把复判过的图像导出成 `<directory>/ok` 和 `<directory>/ng`（后端那台机器上）。回复：`ok`、`ng` 各多少幅，`withoutImage`（复判过但没有图像的：当时没留，或者已经过期） |
| `GET /api/stations/{id}/summary` | | 这个工位有记录以来的汇总：`total`、`ok`、`ng`、`error`、`yield`（良率，OK ÷ (OK + NG)；没有可算的时候是 `null`）、`defectsByCategory`；复判的数字 `reviewed`、`reviewedCorrect`、`falseAlarms`（误报）、`misses`（漏检）、`reviewedErrors` |

工位的回复里：`state`（工位的状态）、`activeRelease`、`pendingRelease` 和 `pendingReady`（待切换的上线包、加载好了没有）、
`runner`（有没有在持续运行、是不是在等新图像、OK / NG / ERROR 各多少次）、`recent`（检测记录：序号、图像名、判定、原因、结果项、用的上线包、是不是刚发生了切换）。

`recent` 是“刚刚检测的”：服务只在内存里给每个工位留最近 100 条，给界面实时显示用。

`records` 是保存在磁盘上的（[说明](../ddd/14-inspection-records.md)），服务重启之后还在。每一条比 `recent` 里的多：
`id`（记录的编号，越新的越大）、`time`（UTC）和 `timeMs`、`recipeVersionId`、`imageState`、`review`（最近一次复判：`truth`、`kind` 是 `correct` / `false_alarm` / `miss` / `was_error`、`note`、`reviewer`、`time`；没复判过是 `null`）。
`imageState` 说的是当时的图像现在的情况（[说明](../ddd/18-image-cache.md)）：`kept` 长期保留着、`cached` 在短期缓存里（默认 72 小时后清掉，复判之后转成长期保留）、`expired` 已经清掉了、`none` 当时就没留。是 `kept` 或 `cached` 时，用 `GET /api/stations/{id}/records/{记录的 id}/image` 取。

调试运行的回复：

```json
{
  "runId": "…",
  "verdict": "NG",
  "totalMs": 5.2,
  "reasons": [{"nodeId": "n5", "pass": false, "text": "缺陷 3 处，上限 0 处"}],
  "result": {
    "defects": [{"nodeId": "n5", "category": "缺陷", "centerX": 170, "centerY": 160,
                 "left": 164, "top": 154, "width": 13, "height": 13, "area": 113}],
    "measurements": [{"nodeId": "n7", "name": "宽度", "value": 400.5, "unit": "像素",
                      "lower": 395, "upper": 405, "pass": true}],
    "poses": [{"nodeId": "n3", "found": true, "score": 0.98, "x": 321.4, "y": 240.2, "angle": 3.1,
               "referenceX": 320, "referenceY": 240}]
  },
  "nodes": [
    {"nodeId": "n3", "operatorId": "vision.threshold", "status": "succeeded", "durationMs": 0.4, "message": "",
     "outputs": {"region": {"type": "region", "hasImage": true, "width": 640, "height": 480,
                            "summary": "region 640x480", "statistics": "…",
                            "imageUrl": "/api/debug-runs/…/outputs/n3/region"}}},
    {"nodeId": "n4", "…": "…",
     "outputs": {"count": {"type": "number", "value": 3, "summary": "number 3.000000"}}}
  ]
}
```

- `result` 是这次检测的**结果项**：发现的缺陷、量出的测量值、定位的结果（[说明](../ddd/10-result-items.md)）。缺陷的坐标是上报它的节点看到的图像上的坐标。
- 数字、布尔值、文本、判定依据、位姿、连通块的值直接在 `value` 里；图像和区域给一个 `imageUrl`，需要时再去取。
- 某个节点失败时：它的 `status` 是 `failed`，`message` 里是原因；它上游的节点照常有输出，下游的是 `skipped`。
- **服务只保留最近 8 次调试运行的输出。** 更早的 `imageUrl` 会得到 404（`api.run_not_found`），重新运行一次即可。

## 3. 一个完整的例子

```bash
B=http://127.0.0.1:8750/api
J="Content-Type: application/json"

curl -s $B/recipes -H "$J" -d '{"name":"亮斑检测"}'          # 记下回复里的 id，下面写作 $R
curl -s $B/recipes/$R/nodes -H "$J" -d '{"operatorId":"vision.image_input"}'    # n1
curl -s $B/recipes/$R/nodes -H "$J" -d '{"operatorId":"vision.to_gray"}'        # n2
curl -s $B/recipes/$R/nodes -H "$J" -d '{"operatorId":"vision.threshold"}'      # n3
curl -s $B/recipes/$R/nodes -H "$J" -d '{"operatorId":"vision.blob"}'           # n4
curl -s $B/recipes/$R/nodes -H "$J" -d '{"operatorId":"vision.judge_count"}'    # n5
curl -s $B/recipes/$R/edges -H "$J" -d '{"from":"n1.image","to":"n2.image"}'
curl -s $B/recipes/$R/edges -H "$J" -d '{"from":"n2.image","to":"n3.image"}'
curl -s $B/recipes/$R/edges -H "$J" -d '{"from":"n3.region","to":"n4.region"}'
curl -s $B/recipes/$R/edges -H "$J" -d '{"from":"n4.blobs","to":"n5.blobs"}'
curl -s -X PUT $B/recipes/$R/nodes/n3/params/value -H "$J" -d '{"value":200}'

curl -s $B/images -H "Content-Type: image/png" --data-binary @某幅图像.png      # 记下 imageId，下面写作 $I
curl -s $B/recipes/$R/debug-runs -H "$J" -d "{\"imageId\":\"$I\"}"
curl -s http://127.0.0.1:8750<回复里某个 imageUrl> -o 中间结果.png
curl -s -X POST $B/recipes/$R/publish
```

做完之后，`vision-cli --data ./vision-data recipe list` 能看到这个方案。

## 4. 代码在哪里

| 内容 | 位置 |
|---|---|
| 接口层（只做翻译，没有业务规则） | `apps/runtime/http_api.cpp` |
| 服务的入口 | `apps/runtime/main.cpp` |
| 两个程序共用的组装 | `apps/common/platform.h` |
| 测试（启动真的服务，用 HTTP 客户端访问） | `tests/http_api_test.cpp` |
