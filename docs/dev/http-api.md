# vision-runtime 的 HTTP 接口

`vision-runtime` 把平台包成一个一直开着的本地服务。它和命令行 `vision-cli` 用的是同一批应用服务、同一个数据目录，所以两边能互相看到对方的方案。

```bash
./build/apps/vision-runtime --data ./vision-data --port 8750
```

```bash
curl http://127.0.0.1:8750/api/health
```

> **没有登录和权限。** 默认只监听本机（`127.0.0.1`）。不要用 `--host` 让它监听不可信的网络。

现在只有方案编排和调试运行这一部分的接口；上线包、工位、训练、模型的接口以后再加。

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
| `POST /api/recipes/{id}/debug-runs` | 请求体 `{"imageId": "…"}`。用草稿当前的样子运行一次 |
| `GET /api/debug-runs/{run}/outputs/{node}/{port}` | 取一个图像或区域输出，PNG。区域是黑白图（选中的像素是白的） |

调试运行的回复：

```json
{
  "runId": "…",
  "verdict": "NG",
  "totalMs": 5.2,
  "reasons": [{"nodeId": "n5", "pass": false, "text": "缺陷 3 处，上限 0 处"}],
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
