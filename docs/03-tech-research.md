# 03 技术调研

| 项 | 内容 |
|---|---|
| 版本 | v0.1 |
| 日期 | 2026-11-11 |
| 说明 | 基于公开资料的初步调研。鸿蒙生态更新很快，标注“待验证”的条目必须由 POC 实测确认 |

每个条目的结论分为三级：✅ 资料充分，基本确定；⚠️ 有资料但不完整，需要验证；❓ 没有找到可靠资料。

---

## 1. 鸿蒙电脑开发环境

| 项 | 结论 | 级别 |
|---|---|---|
| Mac 上开发鸿蒙应用 | DevEco Studio 提供 Mac（ARM）版本 | ✅ |
| 鸿蒙电脑上开发鸿蒙应用 | DevEco Studio、DevEco Code、DevEco CLI 的鸿蒙电脑版已开启公测。要求 **HarmonyOS 7.0.0.107+、16GB 内存、100GB 硬盘**；支持 MateBook 系列和 MatePad Edge | ⚠️ 公测阶段 |
| 设备类型声明 | 应用在 `module.json5` 的 `deviceTypes` 中加入 `2in1` 才能在鸿蒙电脑上运行 | ⚠️ 来自第三方文章 |
| Mac 上的电脑（2in1）模拟器 | 没有找到官方说明；社区有从 Mac 版中提取 2in1 镜像的做法（非官方，不建议依赖） | ❓ |

**对项目的影响**
- 2in1 形态的验证（POC-3b、POC-6、POC-7）需要**鸿蒙电脑真机**。
- 设备到货之前，用 Mac 上的手机或平板模拟器验证 NDK、NAPI、OpenCV 这些与设备形态无关的部分（POC-3a）。
- 购买要求：内存 16GB 以上，能升级到 HarmonyOS 7，型号在 DevEco Studio 鸿蒙电脑版的支持名单里。

## 2. C++ 与 OpenCV 交叉编译

| 项 | 结论 | 级别 |
|---|---|---|
| 鸿蒙 NDK | 提供 Clang 工具链和 CMake，支持 arm64-v8a、x86_64 | ✅ |
| lycium 框架 | OpenHarmony 社区的 C/C++ 三方库交叉编译框架（`tpc_c_cplusplus` 仓库），用 shell 脚本描述构建参数，支持 arm64、armv7、x64，产物可以用于手机、平板和电脑 | ✅ |
| OpenCV 配方 | `tpc_c_cplusplus/thirdparty/opencv/HPKBUILD` 中有现成的构建脚本 | ✅ |
| 构建宿主机 | lycium 推荐 **Ubuntu 22.04**（据称 24.04 效果不佳）。**在 macOS 上直接运行 lycium 没有找到资料** | ⚠️ |
| 华为官方最佳实践 | 华为开发者网站有“使用 lycium 适配 HarmonyOS”的最佳实践文档 | ✅ |

**对项目的影响**
- 方案 A（首选）：用 Linux 容器或虚拟机（Ubuntu 22.04）运行 lycium，编译出 OpenCV 的鸿蒙版产物，放进 `third_party/`。Mac（Apple Silicon）上可以用 Docker 或 OrbStack 运行 arm64 的 Ubuntu。
- 方案 B：直接用 NDK 的 CMake 工具链文件（`ohos.toolchain.cmake`）在 Mac 上编译 OpenCV，只编译需要的模块（core、imgproc、imgcodecs），关闭其他模块。
- 兜底方案：如果 OpenCV 移植问题太多，鸿蒙端改用自研的最小图像库（读写图像可以用系统的 Image Kit）。这会增加工作量，但可行。
- 尽量**裁剪 OpenCV 模块**，减小包体积，降低移植难度。

## 3. Web 编辑器与引擎的桥接（鸿蒙端）

| 项 | 结论 | 级别 |
|---|---|---|
| ArkTS 侧注册 JS 对象 | `javaScriptProxy()`（初始化时）或 `registerJavaScriptProxy()`（初始化后）把 ArkTS 对象注入网页的 `window`；第三个参数是方法白名单；需要配合 `deleteJavaScriptRegister` 防止内存泄漏 | ✅ |
| **C++ 侧直接注册（NDK JSBridge）** | ArkWeb 提供 NDK 接口 `registerJavaScriptProxy` / `registerJavaScriptProxyEx`，**C++ 可以直接与前端页面互相调用**，不需要经过 ArkTS | ✅（官方文档有专门章节） |
| 二进制数据（ArrayBuffer）跨桥传递 | 没有找到前端调用代理方法时传 ArrayBuffer 的说明；`runJavaScriptExt()` 支持 ArrayBuffer，但方向是应用侧向页面执行脚本 | ❓ |
| App 进程内启动本地 HTTP 服务，ArkWeb 访问 127.0.0.1 | 没有找到资料，可能受网络权限或明文 HTTP 限制 | ❓ |

**候选方案（POC-3b 中对比）**

| 方案 | 链路 | 优点 | 风险 |
|---|---|---|---|
| B1：NDK JSBridge | Web JS ⇄ C++ | 链路最短；官方支持 | NDK 接口的使用资料较少 |
| B2：ArkTS 代理 | Web JS ⇄ ArkTS ⇄ NAPI ⇄ C++ | 资料最多 | 多一跳，大数据可能要多次拷贝 |
| B3：进程内 HTTP | Web JS ⇄ HTTP(127.0.0.1) ⇄ C++ | 和 Mac 端完全相同，零桥接代码 | 权限和限制不明 |

**图像传输**：如果桥上不能直接传二进制，就用 Base64。或者把图像写入 App 的沙箱目录，网页通过 ArkWeb 的本地资源拦截（`onInterceptRequest` 一类的机制）按 URL 加载，绕开桥接。POC 中测量 5MP 预览图的端到端延迟。

## 4. 推理：MindSpore Lite

| 项 | 结论 | 级别 |
|---|---|---|
| 系统内置 | 鸿蒙提供 MindSpore Lite Kit，支持 ArkTS 和 Native（C/C++）两种调用方式 | ✅ |
| 模型格式 | 端侧使用 `.ms` 格式；可以用 `converter_lite --fmk=ONNX --modelFile=... --outputFile=...` 从 ONNX 转换 | ✅ |
| 转换环境 | 有开发者反馈转换需要在 **Ubuntu 22.04** 上做，Windows 下失败 | ⚠️ |
| NPU 加速 | 通过 NNRt（Neural Network Runtime）对接麒麟 NPU；具体设备选择和配置代码需要查官方 Native 指南 | ⚠️ |
| 算子兼容性 | 部分 ONNX 算子转换后可能不支持，社区有专门的算子修复工具 | ⚠️ |

**对项目的影响**
- 模型优先选**结构简单、算子常见**的网络（ResNet、MobileNet 一类的骨干网络），降低转换风险。
- 异常检测选 PatchCore 一类方法时，骨干网络推理放在 MindSpore Lite 上，最近邻搜索和打分放在 C++ 里实现。这部分要单独评估内存占用（记忆库大小）。
- 转换脚本和 lycium 共用同一个 Ubuntu 22.04 容器。

## 5. 摄像头（鸿蒙电脑）

| 项 | 结论 | 级别 |
|---|---|---|
| 官方 UVC 支持 | 没有找到华为官方的“外接 UVC 摄像头”开发文档 | ❓ |
| 社区库 | 社区有 `uvc_camera`（ohpm 安装）：可以获取 USB 摄像头列表、预览、设置分辨率和曝光；需要相机权限和 USB 临时权限；**目前是 dev 版本** | ⚠️ |
| 系统 Camera Kit | 笔记本内置摄像头应该可以通过 Camera Kit 使用，外接 USB 摄像头能否被识别为系统相机待验证 | ❓ |

**对项目的影响**
- 摄像头输入定为 P1，P0 只要求文件输入。
- POC-7 依次验证：系统 Camera Kit 能否识别外接 USB 摄像头 → 社区 uvc_camera 库 → 都不行就只支持笔记本内置摄像头和文件输入。

## 6. Mac 端技术栈

| 项 | 选型 | 级别 |
|---|---|---|
| OpenCV | Homebrew `opencv` | ✅ |
| ONNX Runtime | Homebrew `onnxruntime` 或官方预编译包 | ✅ |
| HTTP 服务 | cpp-httplib（单头文件）；WebSocket 另选一个轻量库（如 IXWebSocket），或改用 SSE 推送 | ✅ |
| JSON | nlohmann/json | ✅ |
| 测试 | GoogleTest | ✅ |
| 前端 | Vue 3、Vite、Vue Flow、Konva（vue-konva）、Naive UI | ✅ |

## 7. 风险汇总

| 编号 | 风险 | 影响 | 可能性 | 应对 |
|---|---|---|---|---|
| R-01 | OpenCV 在鸿蒙上编译或运行有问题 | 高 | 中 | lycium 现成配方；裁剪模块；兜底改用自研图像库 |
| R-02 | ArkWeb 桥接性能不足（大图传输） | 中 | 中 | 只传缩略图和局部区域；考虑本地资源拦截的方式 |
| R-03 | 鸿蒙电脑版 DevEco Studio 不稳定（公测中） | 中 | 中 | F-18 定为 P1；主力开发仍在 Mac 上 |
| R-04 | ONNX 转 MindSpore Lite 时算子不兼容 | 中 | 中 | 选结构简单的模型；先用 CPU 跑通 |
| R-05 | 外接 USB 摄像头不可用 | 低 | 中 | 摄像头为 P1；退回内置摄像头或文件输入 |
| R-06 | 没有 2in1 模拟器，设备到货前无法验证电脑形态 | 中 | 高 | 先用手机或平板模拟器验证 NDK 部分；尽早买设备 |
| R-07 | 交叉编译和模型转换都依赖 Linux 环境 | 低 | 高 | 统一使用一个 Ubuntu 22.04 容器，写成脚本 |

## 参考资料

- [用鸿蒙开发鸿蒙：DevEco Studio / Code / CLI 开启鸿蒙PC公测 - 搜狐](https://m.sohu.com/a/1085242681_114760)
- [IT之家：HDC 2026 鸿蒙电脑主流 IDE 上架规划](https://www.ithome.com/0/963/802.htm)
- [Mac 上安装 DevEco Studio - 阿里云开发者社区](https://developer.aliyun.com/article/1634538)
- [tpc_c_cplusplus：OpenCV HPKBUILD](https://gitcode.com/openharmony-sig/tpc_c_cplusplus/blob/master/thirdparty/opencv/HPKBUILD)
- [华为开发者：使用 lycium 适配 HarmonyOS 最佳实践](https://developer.huawei.com/consumer/en/doc/best-practices/bpta-lycium-adapts-to-harmonyos)
- [Qt Wiki：Solutions to build 3rdparty for HarmonyOS/OpenHarmony](https://wiki.qt.io/Solutions_to_build_3rdparty_for_HarmonyOS/OpenHarmony)
- [OpenHarmony 使用 Lycium 交叉编译完整指南 - CSDN](https://bbs.csdn.net/topics/620144465)
- [OpenHarmony 文档：应用与前端页面互相调用（C/C++）](https://gitcode.com/openharmony/docs/blob/master/en/application-dev/web/arkweb-ndk-jsbridge.md)
- [HarmonyOS 文档：前端页面调用应用侧函数](https://harmonyos-docs.pages.dev/arkweb/web-use-frontend-page-js/web-in-page-app-function-invoking/)
- [鸿蒙 Web 组件和内嵌网页双向通信 DEMO - 阿里云](https://developer.aliyun.com/article/1669629)
- [MindSpore Lite Inference Overview](https://www.mindspore.cn/tutorials/en/master/model_infer/lite_infer/overview.html)
- [基于 HarmonyOS 的 YOLO 端侧部署实践 - CSDN](https://damodev.csdn.net/6a43581f662f9a54cb866746.html)
- [MindSpore Lite Kit on HarmonyOS NEXT - DEV Community](https://dev.to/harmonyos/mindspore-lite-kit-on-harmonyos-next-tiny-ai-big-impact-4leh)
- [model-claw：HarmonyOS 模型转换工具集](https://gitcode.com/openharmony-model-agent/model-claw)
- [社区 uvcCamera 库 - Gitee](https://gitee.com/FBFBFB/uvcCamera)
- [HarmonyOS 6.1 支持机型 - 华为官网](https://consumer.huawei.com/cn/support/harmonyos/models/)
