#include "vision/inference_onnx/onnx_model_source.h"

#include <fstream>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "vision/common/path.h"
#include "vision/content/content_store.h"

namespace vision::inference_onnx {

struct OnnxModelSource::Runtime {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "vision"};
};

namespace {

class OnnxModel final : public InferenceModel {
public:
    OnnxModel(ModelInfo info, Ort::Session session, std::string inputName, std::vector<std::string> outputNames)
        : info_(std::move(info)), session_(std::move(session)), inputName_(std::move(inputName)), outputNames_(std::move(outputNames)) {}

    const ModelInfo& info() const override { return info_; }

    Result<std::vector<Tensor>> run(const Tensor& input) const override {
        using R = Result<std::vector<Tensor>>;
        std::int64_t count = 1;
        for (const auto d : input.shape) count *= d;
        if (input.shape.empty() || count <= 0 || static_cast<std::size_t>(count) != input.data.size()) {
            return R::failure("inference.bad_input", "输入张量的形状和数据的长度对不上");
        }
        try {
            const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            // ONNX Runtime 的接口要一个非 const 的指针，但它不会修改输入
            Ort::Value in = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(input.data.data()), input.data.size(),
                                                            input.shape.data(), input.shape.size());
            const char* inputNames[] = {inputName_.c_str()};
            std::vector<const char*> outputNames;
            for (const auto& n : outputNames_) outputNames.push_back(n.c_str());
            // Session::Run 可以被多个线程同时调用（ONNX Runtime 的文档保证）；接口上它不是 const 的
            auto outputs = const_cast<Ort::Session&>(session_).Run(Ort::RunOptions{nullptr}, inputNames, &in, 1, outputNames.data(),
                                                                   outputNames.size());
            std::vector<Tensor> result;
            for (auto& out : outputs) {
                if (!out.IsTensor()) return R::failure("inference.bad_output", "模型的输出不是张量");
                const auto shapeInfo = out.GetTensorTypeAndShapeInfo();
                if (shapeInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                    return R::failure("inference.bad_output", "模型的输出不是 32 位浮点数");
                }
                Tensor t;
                t.shape = shapeInfo.GetShape();
                const float* data = out.GetTensorData<float>();
                t.data.assign(data, data + shapeInfo.GetElementCount());
                result.push_back(std::move(t));
            }
            return result;
        } catch (const Ort::Exception& e) {
            return R::failure("inference.failed", std::string("模型运行失败：") + e.what());
        }
    }

private:
    ModelInfo info_;
    Ort::Session session_;
    std::string inputName_;
    std::vector<std::string> outputNames_;
};

}  // namespace

OnnxModelSource::OnnxModelSource(const models::ModelService& models) : models_(models), runtime_(std::make_unique<Runtime>()) {}
OnnxModelSource::~OnnxModelSource() = default;

Result<std::shared_ptr<const InferenceModel>> OnnxModelSource::load(const std::string& modelVersionId, bool requirePublished) {
    using R = Result<std::shared_ptr<const InferenceModel>>;
    auto view = models_.getModelVersion(modelVersionId);
    if (!view) return R::failure(view.error().code, "找不到模型版本 " + modelVersionId);
    const auto& v = view.value();
    // 状态每次都查：模型加载过之后才发布的，也要能用上
    if (requirePublished && v.status != "published") {
        return R::failure("inference.model_unpublished",
                          "模型版本 " + modelVersionId + " 还没有发布，只能在调试时使用。确认它可用之后先发布（model publish）");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = loaded_.find(modelVersionId); it != loaded_.end()) return it->second;

    // 读出文件并核对哈希
    std::ifstream file(pathFromUtf8(v.filePath), std::ios::binary);
    if (!file) return R::failure("inference.model_file", "模型版本 " + modelVersionId + " 的文件读不出来");
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (content::sha256Hex(bytes) != v.sha256) {
        return R::failure("inference.model_corrupt", "模型版本 " + modelVersionId + " 的文件和登记的内容不一致（被改过或者损坏了）");
    }

    try {
        Ort::SessionOptions options;
        // 每次推理只用一个线程：产线上同时有几条检测流程在跑，线程由检测引擎来安排，推理引擎不要自己再开一堆
        options.SetIntraOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // 从内存里的字节建立会话，不传文件路径：Windows 上路径是宽字符，带中文时容易出错
        Ort::Session session(runtime_->env, bytes.data(), bytes.size(), options);
        if (session.GetInputCount() != 1) {
            return R::failure("inference.unsupported_model", "只支持有一个输入的模型，这个模型有 " + std::to_string(session.GetInputCount()) + " 个");
        }
        Ort::AllocatorWithDefaultOptions allocator;
        std::string inputName = session.GetInputNameAllocated(0, allocator).get();
        std::vector<std::string> outputNames;
        for (std::size_t i = 0; i < session.GetOutputCount(); ++i) outputNames.push_back(session.GetOutputNameAllocated(i, allocator).get());

        ModelInfo info;
        info.id = v.id;
        info.kind = v.contract.kind;
        info.inputWidth = v.contract.inputWidth;
        info.inputHeight = v.contract.inputHeight;
        info.inputChannels = v.contract.inputChannels;
        info.numbers = v.contract.numbers;
        std::shared_ptr<const InferenceModel> model =
            std::make_shared<OnnxModel>(std::move(info), std::move(session), std::move(inputName), std::move(outputNames));
        loaded_[modelVersionId] = model;
        return model;
    } catch (const Ort::Exception& e) {
        return R::failure("inference.load_failed", "模型版本 " + modelVersionId + " 加载失败（它是不是一个 ONNX 文件？）：" + e.what());
    }
}

}  // namespace vision::inference_onnx
