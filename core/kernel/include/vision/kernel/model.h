#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "vision/common/result.h"

namespace vision {

// 张量：一块带形状的浮点数。模型的输入和输出都是它。
struct Tensor {
    std::vector<std::int64_t> shape;  // 例如 {1, 1, 256, 256}：1 幅图像、1 个通道、高 256、宽 256
    std::vector<float> data;          // 按行排列，长度等于 shape 各项的乘积
};

// 模型的说明：算子靠它知道该怎么喂图像、怎么读结果（来自模型仓库里的模型契约）。
struct ModelInfo {
    std::string id;    // 模型版本 ID，例如 "surface-anomaly@v1"
    std::string kind;  // 模型契约的种类，例如 "anomaly-autoencoder"。算子靠它判断这个模型自己会不会用
    int inputWidth = 0, inputHeight = 0, inputChannels = 1;
    std::map<std::string, double> numbers;  // 例如训练时建议的阈值 threshold
};

// 算子拿到手的模型：已经加载好，可以直接运行。
// 算子不知道背后是哪个推理引擎（现在是 ONNX Runtime；鸿蒙上会是 MindSpore Lite）、模型文件在哪里。
// run 必须可以被多个线程同时调用。
class InferenceModel {
public:
    virtual ~InferenceModel() = default;
    virtual const ModelInfo& info() const = 0;
    // 输入一个张量，输出模型的全部输出（顺序是模型导出时定的）。
    virtual Result<std::vector<Tensor>> run(const Tensor& input) const = 0;
};

}  // namespace vision
