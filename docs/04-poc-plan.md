# 04 POC 计划

| 项 | 内容 |
|---|---|
| 版本 | v0.1（评审稿） |
| 日期 | 2026-11-11 |
| 目标 | 用最少的代码验证风险最高的技术假设，**不是开发功能** |

## 1. POC 清单

| 编号 | 验证内容 | 对应风险 / ADR | 环境 | 预估 |
|---|---|---|---|---|
| POC-1 | vision-core：DAG 执行器 + 5 个算子 + 方案 JSON + 命令行运行器 | ADR-04/05/06 | Mac | 3–4 天 |
| POC-3a | OpenCV 和 core 交叉编译到鸿蒙 + NAPI 调用 + ArkUI 显示结果 | R-01、ADR-03 | Mac + Ubuntu 容器 + 手机/平板模拟器 | 4–6 天 |
| POC-3b | ArkWeb 嵌入编辑器原型 + 桥接调用引擎；对比 B1/B2/B3 三种桥接方式 | R-02、ADR-10 | **鸿蒙电脑真机** | 4–5 天 |
| POC-6 | 在鸿蒙电脑上用 DevEco Studio 新增一个算子，编译并运行 | R-03、F-18 | 鸿蒙电脑真机 | 1–2 天 |
| POC-4 | 同一个 ONNX 模型：Mac 上用 ORT，鸿蒙上用 MindSpore Lite（CPU → NPU） | R-04、ADR-11 | Mac + 模拟器 → 真机 | 3–5 天 |
| POC-7 | 鸿蒙电脑采集 USB 摄像头图像 | R-05 | 鸿蒙电脑真机 | 1–2 天 |
| POC-2 | Mac 端：Web 编辑器原型 ↔ vision-server，端到端运行并查看中间图像 | ADR-07/09 | Mac | 4–5 天 |

## 2. 执行顺序

```
POC-1 ──► POC-3a ──►（设备到货）──► POC-3b ──► POC-6 ──► POC-4 ──► POC-7
                  └──────────────► POC-2（在等设备期间穿插进行）
```

- 风险高的鸿蒙项（3a、3b、4）尽早做，走不通可以尽早调整方案。
- POC-2 是常规 Web 开发，风险低，安排在等待设备期间完成。
- 总时长约 4–5 周（按业余时间估算）。

## 3. 各 POC 详细说明

### POC-1：vision-core 骨架

**范围**
- 数据类型：`Image`、`Region`、`BlobList`、`Number`、`Bool`
- 算子：`load_image`、`to_gray`、`threshold`、`blob`、`judge`
- 注册表、DAG 构建与校验（类型、环、必填输入）、拓扑排序、单线程执行、记录各节点耗时
- 方案 JSON 的读写
- 命令行：`vision-run <recipe.json> <image>`，输出判定结果和各节点耗时；`--dump-dir` 导出中间结果图

**通过标准**
- [ ] 示例方案在 MVTec AD 的若干张图上运行，判定结果符合预期
- [ ] 有环、类型不匹配、缺少输入的方案都能报出清楚的错误信息
- [ ] 新增一个算子只改 1 个文件
- [ ] 单元测试全部通过；core 不链接任何 UI 库

### POC-3a：鸿蒙交叉编译与 NAPI

**范围**
- 在 Ubuntu 22.04 容器中用 lycium 编译 OpenCV（只编译 core、imgproc、imgcodecs）；同时尝试在 Mac 上直接用 NDK 的 CMake 编译，记录两种方式的结果
- 鸿蒙工程的 CMake 引用 `core/`，编译出 `libvision.so`
- NAPI 导出 `listOperators()`、`runRecipe(recipeJson, imagePath)`
- ArkUI 页面：选择图片 → 调用 → 显示判定结果和结果图

**通过标准**
- [ ] 在模拟器上运行 POC-1 的同一个方案，判定结果和 Mac 端一致
- [ ] 记录 OpenCV 产物体积和 App 安装包体积
- [ ] 交叉编译过程写成可以重复执行的脚本

### POC-3b：ArkWeb 嵌入编辑器与桥接

**范围**
- 一个极简的网页：调用 `listOperators`，运行方案，显示结果图
- 分别用 B1（NDK JSBridge）、B2（ArkTS 代理 + NAPI）、B3（进程内 HTTP）实现（B3 不可行就跳过）
- 图像传输对比：Base64 和本地资源拦截两种方式

**通过标准**
- [ ] 至少一种方案端到端可用
- [ ] 5MP 原图生成 1600px 预览并显示在网页上，延迟 < 300 ms
- [ ] 输出各方案的对比表，确定 ADR-10

### POC-6：在鸿蒙电脑上开发算子

**通过标准**
- [ ] 在鸿蒙电脑上用 DevEco Studio 打开工程，新增一个算子（如 `invert`），编译、运行、在编辑器中能看到它
- [ ] 记录完整的步骤和耗时，作为 F-18 的可行性结论

### POC-4：推理一致性

**范围**
- 模型：先用 ResNet18 分类模型（算子常见，降低转换风险）；通过后再尝试异常检测的骨干网络
- Mac：ONNX Runtime；鸿蒙：`converter_lite` 转成 `.ms` 格式，用 MindSpore Lite 的 Native API 推理

**通过标准**
- [ ] 同一张图在两端的输出最大误差 < 1e-3
- [ ] 记录 CPU 推理耗时；真机上记录 NPU 推理耗时（如果可用）

### POC-7：USB 摄像头

**通过标准**
- [ ] 依次尝试系统 Camera Kit、社区 uvc_camera 库，得到可以送入引擎的帧
- [ ] 结论写进 03 调研 §5

### POC-2：Mac 端 Web ↔ 引擎

**范围**
- vision-server：实现 `/api/operators`、`/api/run`、节点输出接口
- Web：Vue Flow 画布（从算子列表拖入节点、连线）、自动生成的参数面板、结果图查看
- 实现 `EngineClient` 接口和 `HttpWsClient`

**通过标准**
- [ ] 在浏览器中搭出“灰度 → 阈值 → Blob → 判定”流程并运行，点击节点可以查看中间结果
- [ ] 预览延迟 < 300 ms
- [ ] 修改算子参数后重新运行，结果正确更新

## 4. POC 交付物

1. POC 代码（保存在仓库中，不追求质量，但要能运行）
2. 各 POC 的测量数据（耗时、误差、包体积）
3. 《POC 结论报告》：每个假设是否成立，ADR 状态更新，需求或架构是否需要调整
