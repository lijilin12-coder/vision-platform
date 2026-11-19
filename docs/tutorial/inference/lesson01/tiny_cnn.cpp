// 一个最小的推理引擎：用第 1 课的算子搭一个小卷积网络，跑一次前向，并给每一层计时。
// 权重是随机数（固定种子），所以“分类结果”没有意义；这里看的是数据怎么流动、时间花在哪里。
//
// 一定要用优化模式编译：
//   macOS / Linux：clang++ -std=c++17 -O2 -I../common tiny_cnn.cpp -o tiny_cnn && ./tiny_cnn
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 /I..\common tiny_cnn.cpp && tiny_cnn.exe
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "inftut/common.h"
#include "ops.h"

using inftut::Tensor;

namespace {

// 把结果“用掉”，防止编译器发现结果没人用而把整段计算删掉（C++ 教程第 14 课）。
volatile float sink = 0;

// 网络的全部权重。真实的模型里，这些数是训练得到的，保存在模型文件里。
struct Weights {
    Tensor conv1W, conv1B;  // 3 → 16 通道，3×3
    Tensor conv2W, conv2B;  // 16 → 32 通道，3×3
    Tensor conv3W, conv3B;  // 32 → 64 通道，3×3
    Tensor fcW, fcB;        // 64*8*8 → 10
};

Weights makeWeights() {
    inftut::Rng rng(1);
    Weights w;
    // 标准差取 sqrt(2 / 每个输出元素的输入个数)：这样每一层输出的大小和输入差不多，不会越算越大或越小
    w.conv1W = inftut::randomTensor(rng, 16, 3, 3, 3, std::sqrt(2.0f / (3 * 9)));
    w.conv1B = Tensor(1, 16, 1, 1);
    w.conv2W = inftut::randomTensor(rng, 32, 16, 3, 3, std::sqrt(2.0f / (16 * 9)));
    w.conv2B = Tensor(1, 32, 1, 1);
    w.conv3W = inftut::randomTensor(rng, 64, 32, 3, 3, std::sqrt(2.0f / (32 * 9)));
    w.conv3B = Tensor(1, 64, 1, 1);
    w.fcW = inftut::randomTensor(rng, 10, 64 * 8 * 8, 1, 1, std::sqrt(1.0f / (64 * 8 * 8)));
    w.fcB = Tensor(1, 10, 1, 1);
    return w;
}

// 一次完整的前向计算：从图像到 10 个类别的概率。
Tensor forward(const Weights& w, const Tensor& image) {
    Tensor x = inftut::conv2d(image, w.conv1W, w.conv1B, 1, 1);  // 1×16×64×64
    x = inftut::relu(x);
    x = inftut::maxPool(x, 2);                                   // 1×16×32×32
    x = inftut::conv2d(x, w.conv2W, w.conv2B, 1, 1);             // 1×32×32×32
    x = inftut::relu(x);
    x = inftut::maxPool(x, 2);                                   // 1×32×16×16
    x = inftut::conv2d(x, w.conv3W, w.conv3B, 1, 1);             // 1×64×16×16
    x = inftut::relu(x);
    x = inftut::maxPool(x, 2);                                   // 1×64×8×8
    x = inftut::linear(x, w.fcW, w.fcB);                         // 1×10×1×1
    return inftut::softmax(x);
}

struct LayerTime {
    std::string name;
    std::string outShape;
    double ms;
};

// 和 forward 做的事情完全一样，只是每一层单独计时（重复多次取中位数）。
std::vector<LayerTime> timeLayers(const Weights& w, const Tensor& image) {
    const int repeats = 15;
    std::vector<LayerTime> times;
    Tensor x = image;
    // 给一层计时：把它运行 repeats 次取中位数，然后用结果作为下一层的输入
    auto layer = [&](const std::string& name, auto&& fn) {
        Tensor y;
        const double ms = inftut::medianMs(repeats, [&] {
            y = fn(x);
            sink = y.data[0];
        });
        times.push_back({name, y.shapeText(), ms});
        x = std::move(y);
    };
    layer("conv1", [&](const Tensor& in) { return inftut::conv2d(in, w.conv1W, w.conv1B, 1, 1); });
    layer("relu1", [&](const Tensor& in) { return inftut::relu(in); });
    layer("pool1", [&](const Tensor& in) { return inftut::maxPool(in, 2); });
    layer("conv2", [&](const Tensor& in) { return inftut::conv2d(in, w.conv2W, w.conv2B, 1, 1); });
    layer("relu2", [&](const Tensor& in) { return inftut::relu(in); });
    layer("pool2", [&](const Tensor& in) { return inftut::maxPool(in, 2); });
    layer("conv3", [&](const Tensor& in) { return inftut::conv2d(in, w.conv3W, w.conv3B, 1, 1); });
    layer("relu3", [&](const Tensor& in) { return inftut::relu(in); });
    layer("pool3", [&](const Tensor& in) { return inftut::maxPool(in, 2); });
    layer("fc", [&](const Tensor& in) { return inftut::linear(in, w.fcW, w.fcB); });
    layer("softmax", [&](const Tensor& in) { return inftut::softmax(in); });
    return times;
}

}  // namespace

int main() {
    const Weights weights = makeWeights();

    // 一张 64×64 的三通道“图像”，像素值在 0～1 之间
    inftut::Rng rng(2);
    Tensor image(1, 3, 64, 64);
    for (float& v : image.data) v = rng.uniform();

    const Tensor probs = forward(weights, image);

    std::printf("输入 %s，输出 %s\n", image.shapeText().c_str(), probs.shapeText().c_str());
    std::printf("10 个类别的概率：");
    float total = 0;
    int best = 0;
    for (int i = 0; i < probs.c; ++i) {
        std::printf("%.3f ", static_cast<double>(probs.at(0, i, 0, 0)));
        total += probs.at(0, i, 0, 0);
        if (probs.at(0, i, 0, 0) > probs.at(0, best, 0, 0)) best = i;
    }
    std::printf("\n概率之和 %.4f，最大的是第 %d 类\n\n", static_cast<double>(total), best);

    const std::vector<LayerTime> times = timeLayers(weights, image);
    double all = 0;
    double conv = 0;
    for (const LayerTime& t : times) {
        all += t.ms;
        if (t.name.rfind("conv", 0) == 0) conv += t.ms;
    }
    std::printf("层       输出形状        耗时 ms    占比\n");
    for (const LayerTime& t : times)
        std::printf("%-8s %-12s %10.3f %6.1f%%\n", t.name.c_str(), t.outShape.c_str(), t.ms, 100 * t.ms / all);
    std::printf("合计 %.2f ms，其中三个卷积层占 %.1f%%\n", all, 100 * conv / all);

    // 自检：概率加起来必须是 1，而且每个都是有效的数
    bool ok = std::fabs(total - 1.0f) < 1e-4f;
    for (float p : probs.data) ok = ok && p >= 0 && p <= 1;
    if (!ok) {
        std::printf("错误：输出不是一组有效的概率\n");
        return 1;
    }
    return 0;
}
