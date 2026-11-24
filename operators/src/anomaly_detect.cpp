// 异常检测：用一个训练好的模型判断图像“像不像合格的”，输出异常得分和异常图。
//
// 模型来自模型仓库（参数 model 是模型版本的 ID），由训练服务训练出来（services/training）。
// 这个算子只认一种模型契约：anomaly-autoencoder——
//   输入   [1, 1, 高, 宽]，灰度，0～1
//   输出 0 [1]             异常得分
//   输出 1 [1, 1, 高, 宽]   异常图
//
// 【预处理必须和训练时完全一样】：缩放用 INTER_AREA，然后除以 255。
// 对应的训练代码是 services/training/trainer.py 的 load_image；改这里就必须同时改那里。
// （实测过：两边缩放的算法不一样时，同一幅合格图像的得分差 70%，直接越过阈值。）
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

#include "op_util.h"

namespace vision::operators {

class AnomalyDetectOp final : public catalog::Operator {
public:
    static constexpr const char* kModelKind = "anomaly-autoencoder";

    static catalog::OperatorMeta meta() {
        catalog::OperatorMeta m;
        m.signature.id = "vision.anomaly_detect";
        m.signature.majorVersion = 1;
        m.signature.inputs = {{"image", PortType::Image, false}};
        m.signature.outputs = {{"ratio", PortType::Number, false}, {"score", PortType::Number, false}, {"map", PortType::Image, false}};
        m.signature.params = {
            {"model", ParamType::Model, std::string(""), std::nullopt, std::nullopt, {}, "模型版本（例如 surface-anomaly@v1）"},
        };
        m.displayName = "异常检测";
        m.category = "深度学习";
        m.description =
            "用训练好的模型判断图像像不像合格的。ratio 是异常得分除以模型建议的阈值：大于 1 表示不正常，"
            "后面接“范围判定”（最大值 1）即可。map 是异常图：越亮越不正常，128 对应阈值";
        return m;
    }

    Status execute(catalog::OpContext& ctx) override {
        const char* op = "operator.anomaly_detect";
        auto in = grayInput(ctx, "image", op, "异常检测");
        if (!in) return in.error();
        auto loaded = ctx.model("model");
        if (!loaded) return loaded.error();
        const InferenceModel& model = *loaded.value();
        const ModelInfo& info = model.info();
        if (info.kind != kModelKind) {
            return Status::failure(op, "模型 " + info.id + " 的种类是 " + info.kind + "，这个算子只会用 " + kModelKind);
        }
        if (info.inputWidth <= 0 || info.inputHeight <= 0 || info.inputChannels != 1) {
            return Status::failure(op, "模型 " + info.id + " 的契约不完整：输入尺寸不对");
        }
        // 模型是用整幅图像训练的；只看一部分的话，它看到的东西和训练时不一样，得分没有意义
        if (ctx.roi()) return Status::failure(op, "异常检测不支持 ROI：模型是用整幅图像训练的");
        const auto threshold = info.numbers.find("threshold");
        if (threshold == info.numbers.end() || !(threshold->second > 0)) {
            return Status::failure(op, "模型 " + info.id + " 的契约里没有建议的阈值（threshold）");
        }

        // ---- 预处理（和训练时一致）----
        const cv::Mat gray = imaging::view(in.value());
        cv::Mat resized;
        cv::resize(gray, resized, cv::Size(info.inputWidth, info.inputHeight), 0, 0, cv::INTER_AREA);
        Tensor input;
        input.shape = {1, 1, info.inputHeight, info.inputWidth};
        input.data.resize(static_cast<std::size_t>(info.inputWidth) * info.inputHeight);
        // target 只是 input.data 的一个“视图”：convertTo 直接把结果写进张量，不用再复制一次
        cv::Mat target(info.inputHeight, info.inputWidth, CV_32F, input.data.data());
        resized.convertTo(target, CV_32F, 1.0 / 255.0);

        // ---- 推理 ----
        auto outputs = model.run(input);
        if (!outputs) return outputs.error();
        const auto& out = outputs.value();
        const std::size_t pixels = input.data.size();
        if (out.size() < 2 || out[0].data.size() != 1 || out[1].data.size() != pixels) {
            return Status::failure(op, "模型 " + info.id + " 的输出和 " + kModelKind + " 的约定不一致");
        }
        const double score = out[0].data[0];
        if (!std::isfinite(score)) return Status::failure(op, "模型 " + info.id + " 算出的得分不是有效的数");

        // ---- 异常图：放大回原图的尺寸，128 对应阈值 ----
        const cv::Mat mapSmall(info.inputHeight, info.inputWidth, CV_32F, const_cast<float*>(out[1].data.data()));
        cv::Mat map8, mapFull;
        mapSmall.convertTo(map8, CV_8U, 128.0 / threshold->second);  // 超过 255 的自动截断
        cv::resize(map8, mapFull, gray.size(), 0, 0, cv::INTER_LINEAR);
        auto mapImage = imaging::fromMat(mapFull);
        if (!mapImage) return mapImage.error();

        ctx.setOutput("score", score);
        ctx.setOutput("ratio", score / threshold->second);
        ctx.setOutput("map", std::move(mapImage).value());
        return Status::success();
    }
};

}  // namespace vision::operators

using vision::operators::AnomalyDetectOp;
VISION_REGISTER_OPERATOR(AnomalyDetectOp)
