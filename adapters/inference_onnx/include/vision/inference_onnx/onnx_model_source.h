#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "vision/execution/api.h"
#include "vision/models/api.h"

namespace vision::inference_onnx {

// 用 ONNX Runtime 运行模型仓库里的模型：实现检测执行的 ModelSource 端口。
//
// 加载一个模型版本时：
//   1. 向模型仓库查它的状态、契约、文件位置
//   2. 读出模型文件，核对内容的哈希（文件被改过或者损坏时拒绝加载，而不是悄悄运行一个不对的模型）
//   3. 交给 ONNX Runtime 建立会话
// 加载过的模型留在内存里（模型版本不可变，所以不用担心它变旧）；同一个模型被几个方案用到，只加载一次。
class OnnxModelSource final : public execution::ModelSource {
public:
    explicit OnnxModelSource(const models::ModelService& models);
    ~OnnxModelSource() override;

    Result<std::shared_ptr<const InferenceModel>> load(const std::string& modelVersionId, bool requirePublished) override;

private:
    struct Runtime;  // ONNX Runtime 的全局环境。头文件里不出现 ONNX Runtime 的任何类型
    const models::ModelService& models_;
    std::unique_ptr<Runtime> runtime_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const InferenceModel>> loaded_;
};

}  // namespace vision::inference_onnx
