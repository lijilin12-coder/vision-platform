// 把一个小的分类模型从 FP32 量化成 INT8，用整数运算推理，比较精度和大小。
//
//   模型：64 维输入 → 全连接（128）→ ReLU → 全连接（10 类）
//   流程：训练（FP32）→ 用校准集确定每一层输入的范围 → 量化权重 → 整数推理 → 在测试集上比精度
//
//   macOS / Linux：clang++ -std=c++17 -O2 -I../common quant_mlp.cpp -o quant_mlp && ./quant_mlp
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 /I..\common quant_mlp.cpp && quant_mlp.exe
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "inftut/common.h"
#include "quant.h"

namespace {

constexpr int kInput = 64;
constexpr int kHidden = 128;
constexpr int kClasses = 10;
constexpr float kNoise = 1.8f;

// ====================================================================================
// 第一部分：准备数据和一个训练好的 FP32 模型。这不是本课的重点，可以先跳过，从“第二部分”读起。
// ====================================================================================

struct Sample {
    std::vector<float> x;
    int label = 0;
};

// 构造的分类任务：每个类别有一个“原型”向量，样本 = 原型 + 噪声。
std::vector<Sample> makeData(int count, const std::vector<std::vector<float>>& prototypes, inftut::Rng& rng) {
    std::vector<Sample> data;
    for (int i = 0; i < count; ++i) {
        Sample s;
        s.label = static_cast<int>(rng.below(kClasses));
        for (int d = 0; d < kInput; ++d) {
            s.x.push_back(prototypes[static_cast<std::size_t>(s.label)][static_cast<std::size_t>(d)] + kNoise * rng.normal());
        }
        data.push_back(std::move(s));
    }
    return data;
}

struct Linear {
    int in = 0, out = 0;
    std::vector<float> weight;  // out 行 × in 列
    std::vector<float> bias;    // out 个

    Linear(int inSize, int outSize, inftut::Rng& rng) : in(inSize), out(outSize) {
        const float stddev = std::sqrt(2.0f / static_cast<float>(in));
        for (int i = 0; i < in * out; ++i) weight.push_back(rng.normal() * stddev);
        bias.assign(static_cast<std::size_t>(out), 0.0f);
    }

    std::vector<float> forward(const std::vector<float>& x) const {
        std::vector<float> y(bias);
        for (int o = 0; o < out; ++o)
            for (int i = 0; i < in; ++i) y[static_cast<std::size_t>(o)] += weight[static_cast<std::size_t>(o * in + i)] * x[static_cast<std::size_t>(i)];
        return y;
    }
};

struct Mlp {
    Linear fc1;
    Linear fc2;

    explicit Mlp(inftut::Rng& rng) : fc1(kInput, kHidden, rng), fc2(kHidden, kClasses, rng) {}

    // hidden：ReLU 之后的隐藏层（校准时要用）
    std::vector<float> forward(const std::vector<float>& x, std::vector<float>* hidden = nullptr) const {
        std::vector<float> h = fc1.forward(x);
        for (float& v : h) v = std::max(v, 0.0f);
        if (hidden != nullptr) *hidden = h;
        return fc2.forward(h);
    }
};

int argmax(const std::vector<float>& v) { return static_cast<int>(std::max_element(v.begin(), v.end()) - v.begin()); }

// 最普通的随机梯度下降，一次一个样本。
void train(Mlp& model, const std::vector<Sample>& data, int epochs, float lr) {
    for (int epoch = 0; epoch < epochs; ++epoch) {
        for (const Sample& s : data) {
            std::vector<float> h;
            std::vector<float> logits = model.forward(s.x, &h);
            // softmax + 交叉熵的梯度：概率 − 标签
            const float top = *std::max_element(logits.begin(), logits.end());
            float sum = 0;
            for (float& v : logits) sum += (v = std::exp(v - top));
            for (float& v : logits) v /= sum;
            logits[static_cast<std::size_t>(s.label)] -= 1.0f;

            std::vector<float> gradHidden(kHidden, 0.0f);
            for (int o = 0; o < kClasses; ++o) {
                const float g = logits[static_cast<std::size_t>(o)];
                for (int i = 0; i < kHidden; ++i) {
                    gradHidden[static_cast<std::size_t>(i)] += g * model.fc2.weight[static_cast<std::size_t>(o * kHidden + i)];
                    model.fc2.weight[static_cast<std::size_t>(o * kHidden + i)] -= lr * g * h[static_cast<std::size_t>(i)];
                }
                model.fc2.bias[static_cast<std::size_t>(o)] -= lr * g;
            }
            for (int o = 0; o < kHidden; ++o) {
                if (h[static_cast<std::size_t>(o)] <= 0) continue;  // ReLU 的梯度
                const float g = gradHidden[static_cast<std::size_t>(o)];
                for (int i = 0; i < kInput; ++i) model.fc1.weight[static_cast<std::size_t>(o * kInput + i)] -= lr * g * s.x[static_cast<std::size_t>(i)];
                model.fc1.bias[static_cast<std::size_t>(o)] -= lr * g;
            }
        }
    }
}

// ====================================================================================
// 第二部分：量化
// ====================================================================================

// 量化之后的全连接层。只保存整数和几个 scale。
struct QLinear {
    int in = 0, out = 0;
    std::vector<std::int8_t> weight;   // 对称量化，每一行（每个输出通道）一个 scale
    std::vector<float> weightScale;    // out 个
    std::vector<std::int32_t> bias;    // 偏置用 32 位整数，scale = 输入的 scale × 这一行权重的 scale
    quant::QParams input;              // 输入激活值的量化参数（uint8，非对称）

    // 整数推理的核心：只有整数的乘法和加法。
    //   真实值：  y = Σ w·x + b
    //   代入      w ≈ sw·qw，  x ≈ sx·(qx − zx)，  b ≈ sw·sx·qb
    //   得到      y ≈ sw·sx · [ Σ qw·(qx − zx) + qb ]
    // 方括号里全是整数，这就是 acc。
    std::vector<std::int32_t> accumulate(const std::vector<std::uint8_t>& qx) const {
        std::vector<std::int32_t> acc(bias);
        for (int o = 0; o < out; ++o) {
            std::int32_t sum = 0;
            const std::int8_t* row = weight.data() + static_cast<std::size_t>(o) * static_cast<std::size_t>(in);
            for (int i = 0; i < in; ++i) sum += static_cast<std::int32_t>(row[i]) * (static_cast<std::int32_t>(qx[static_cast<std::size_t>(i)]) - input.zeroPoint);
            acc[static_cast<std::size_t>(o)] += sum;
        }
        return acc;
    }

    // acc 代表的实数值
    float realValue(std::int32_t acc, int o) const { return weightScale[static_cast<std::size_t>(o)] * input.scale * static_cast<float>(acc); }

    std::size_t bytes() const { return weight.size() + bias.size() * 4 + weightScale.size() * 4 + 8; }
};

// weightBits：权重用几位整数表示（8 就是 INT8）。perChannel：每行一个 scale，还是整个矩阵一个。
QLinear quantizeLinear(const Linear& fp, quant::QParams input, int weightBits, bool perChannel) {
    QLinear q;
    q.in = fp.in;
    q.out = fp.out;
    q.input = input;
    float globalMax = 0;
    for (float w : fp.weight) globalMax = std::max(globalMax, std::fabs(w));
    for (int o = 0; o < fp.out; ++o) {
        float rowMax = 0;
        for (int i = 0; i < fp.in; ++i) rowMax = std::max(rowMax, std::fabs(fp.weight[static_cast<std::size_t>(o * fp.in + i)]));
        const float scale = quant::chooseSymmetricScale(perChannel ? rowMax : globalMax, weightBits);
        q.weightScale.push_back(scale);
        for (int i = 0; i < fp.in; ++i) q.weight.push_back(quant::quantizeS8(fp.weight[static_cast<std::size_t>(o * fp.in + i)], scale, weightBits));
        q.bias.push_back(static_cast<std::int32_t>(std::lround(fp.bias[static_cast<std::size_t>(o)] / (scale * input.scale))));
    }
    return q;
}

struct QMlp {
    QLinear fc1;
    QLinear fc2;

    int predict(const std::vector<float>& x) const {
        // 1. 输入：实数 → uint8
        std::vector<std::uint8_t> qx;
        for (float v : x) qx.push_back(quant::quantizeU8(v, fc1.input));

        // 2. 第一层：整数累加，然后“重新量化”成第二层输入的 uint8
        //    实数值 = sw·sx·acc，再除以下一层的 scale 并加上 zeroPoint。
        //    ReLU 不需要单独做：下一层的范围从 0 开始，小于 0 的值在截断到 [0, 255] 时自然变成 0。
        const std::vector<std::int32_t> acc1 = fc1.accumulate(qx);
        std::vector<std::uint8_t> qh;
        for (int o = 0; o < fc1.out; ++o) qh.push_back(quant::quantizeU8(fc1.realValue(acc1[static_cast<std::size_t>(o)], o), fc2.input));

        // 3. 第二层：整数累加，换算回实数后取最大的
        const std::vector<std::int32_t> acc2 = fc2.accumulate(qh);
        std::vector<float> logits;
        for (int o = 0; o < fc2.out; ++o) logits.push_back(fc2.realValue(acc2[static_cast<std::size_t>(o)], o));
        return argmax(logits);
    }

    std::size_t bytes() const { return fc1.bytes() + fc2.bytes(); }
};

enum class Calibration { MinMax, Percentile };

// 校准：让 FP32 模型把校准集跑一遍，记下每一层输入的全部取值，据此确定范围。
QMlp quantize(const Mlp& model, const std::vector<Sample>& calibrationSet, Calibration method, int weightBits,
              bool perChannel) {
    std::vector<float> inputs, hiddens;
    for (const Sample& s : calibrationSet) {
        std::vector<float> h;
        model.forward(s.x, &h);
        inputs.insert(inputs.end(), s.x.begin(), s.x.end());
        hiddens.insert(hiddens.end(), h.begin(), h.end());
    }
    const auto rangeOf = [method](const std::vector<float>& values) {
        return method == Calibration::MinMax ? quant::minMax(values) : quant::percentile(values, 99.9);
    };
    const quant::QParams inputParams = quant::chooseAffine(rangeOf(inputs));
    const quant::QParams hiddenParams = quant::chooseAffine(rangeOf(hiddens));
    return QMlp{quantizeLinear(model.fc1, inputParams, weightBits, perChannel),
                quantizeLinear(model.fc2, hiddenParams, weightBits, perChannel)};
}

template <typename Predict>
double accuracy(const std::vector<Sample>& data, Predict&& predict) {
    int correct = 0;
    for (const Sample& s : data) correct += predict(s.x) == s.label;
    return 100.0 * correct / static_cast<double>(data.size());
}

}  // namespace

int main() {
    inftut::Rng rng(2026);
    std::vector<std::vector<float>> prototypes(kClasses);
    for (auto& p : prototypes)
        for (int d = 0; d < kInput; ++d) p.push_back(rng.normal());

    const std::vector<Sample> trainSet = makeData(4000, prototypes, rng);
    std::vector<Sample> calibrationSet = makeData(100, prototypes, rng);  // 校准集：100 个样本，不需要标签
    // 校准集里混进了一个坏样本：有三个值大得离谱（比如传感器坏点、图像过曝）。训练集和测试集里没有。
    calibrationSet[17].x[5] = 400.0f;
    calibrationSet[17].x[40] = -250.0f;
    calibrationSet[17].x[41] = 320.0f;
    const std::vector<Sample> testSet = makeData(4000, prototypes, rng);

    Mlp model(rng);
    train(model, trainSet, 8, 0.003f);

    const double fpAccuracy = accuracy(testSet, [&](const std::vector<float>& x) { return argmax(model.forward(x)); });
    const std::size_t fpBytes = (model.fc1.weight.size() + model.fc1.bias.size() + model.fc2.weight.size() + model.fc2.bias.size()) * 4;
    std::printf("测试集 %zu 个样本，校准集 %zu 个样本\n\n", testSet.size(), calibrationSet.size());
    std::printf("  %-34s 精度      参数大小\n", "");
    std::printf("  FP32                               %5.2f%%   %6zu 字节\n", fpAccuracy, fpBytes);

    const QMlp minMax = quantize(model, calibrationSet, Calibration::MinMax, 8, true);
    const QMlp percentile = quantize(model, calibrationSet, Calibration::Percentile, 8, true);
    const double minMaxAccuracy = accuracy(testSet, [&](const std::vector<float>& x) { return minMax.predict(x); });
    const double percentileAccuracy = accuracy(testSet, [&](const std::vector<float>& x) { return percentile.predict(x); });
    std::printf("  INT8，最大最小值校准               %5.2f%%   %6zu 字节\n", minMaxAccuracy, minMax.bytes());
    std::printf("  INT8，99.9 百分位校准              %5.2f%%   %6zu 字节（FP32 的 %.0f%%）\n\n", percentileAccuracy,
                percentile.bytes(), 100.0 * static_cast<double>(percentile.bytes()) / static_cast<double>(fpBytes));

    std::printf("权重的位数越少，精度掉得越多（激活值仍然是 8 位，99.9 百分位校准）：\n");
    std::printf("  权重位数   整个矩阵一个 scale   每行一个 scale\n");
    for (int bits : {8, 6, 4, 3, 2}) {
        const QMlp tensor = quantize(model, calibrationSet, Calibration::Percentile, bits, false);
        const QMlp channel = quantize(model, calibrationSet, Calibration::Percentile, bits, true);
        const double a = accuracy(testSet, [&](const std::vector<float>& x) { return tensor.predict(x); });
        const double b = accuracy(testSet, [&](const std::vector<float>& x) { return channel.predict(x); });
        std::printf("  %d          %5.2f%%               %5.2f%%\n", bits, a, b);
    }

    // 作为测试运行时检查的几件事
    int failures = 0;
    if (fpAccuracy < 85.0) ++failures, std::printf("FP32 模型没有训练好\n");
    if (percentileAccuracy < fpAccuracy - 1.5) ++failures, std::printf("INT8（百分位校准）的精度不应该比 FP32 低这么多\n");
    if (minMaxAccuracy > percentileAccuracy) ++failures, std::printf("这份数据上，百分位校准应该不比最大最小值差\n");
    if (failures > 0) return EXIT_FAILURE;
    std::printf("\n检查通过：INT8 的精度和 FP32 接近\n");
    return EXIT_SUCCESS;
}
