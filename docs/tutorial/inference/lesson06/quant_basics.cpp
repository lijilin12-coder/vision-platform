// 量化的基本概念：scale 和 zeroPoint、对称与非对称、校准方法、逐通道量化。
// 每一段都用数字说明“选得不好会损失多少”。
//
//   macOS / Linux：clang++ -std=c++17 -I../common quant_basics.cpp -o quant_basics && ./quant_basics
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common quant_basics.cpp && quant_basics.exe
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

#include "inftut/common.h"
#include "quant.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  [失败] %s\n", what);
        ++failures;
    }
}

std::vector<float> roundTripU8(const std::vector<float>& values, quant::QParams p) {
    std::vector<float> out;
    for (float v : values) out.push_back(quant::dequantize(quant::quantizeU8(v, p), p));
    return out;
}

std::vector<float> roundTripS8(const std::vector<float>& values, float scale, int bits = 8) {
    std::vector<float> out;
    for (float v : values) out.push_back(scale * static_cast<float>(quant::quantizeS8(v, scale, bits)));
    return out;
}

// ---------- 1. 一个数的量化和反量化 ----------

void oneNumber() {
    std::printf("1. 把 [-1.0, 3.0] 量化成 uint8：\n");
    const quant::QParams p = quant::chooseAffine({-1.0f, 3.0f});
    std::printf("   scale = %.6f，zeroPoint = %d\n", static_cast<double>(p.scale), p.zeroPoint);
    for (float r : {-1.0f, 0.0f, 0.5f, 1.2345f, 3.0f, 5.0f}) {
        const int q = quant::quantizeU8(r, p);
        const float back = quant::dequantize(q, p);
        std::printf("   %7.4f → %3d → %7.4f   误差 %+.4f%s\n", static_cast<double>(r), q, static_cast<double>(back),
                    static_cast<double>(back - r), r > 3.0f ? "   ← 超出范围，被截断" : "");
        if (r >= -1.0f && r <= 3.0f) check(std::fabs(back - r) <= p.scale / 2 + 1e-6f, "范围内的误差不超过半个 scale");
    }
    check(quant::dequantize(p.zeroPoint, p) == 0.0f, "实数 0 被精确表示");
    std::printf("\n");
}

// ---------- 2. 对称还是非对称 ----------

void symmetricVsAffine() {
    // ReLU 之后的激活值：没有负数
    inftut::Rng rng(1);
    std::vector<float> activations;
    for (int i = 0; i < 10000; ++i) activations.push_back(std::max(0.0f, rng.normal() * 2.0f));
    const quant::Range range = quant::minMax(activations);

    const float symScale = quant::chooseSymmetricScale(range.hi);
    const quant::QParams affine = quant::chooseAffine(range);
    const double symDb = quant::sqnrDb(activations, roundTripS8(activations, symScale));
    const double affineDb = quant::sqnrDb(activations, roundTripU8(activations, affine));

    std::printf("2. ReLU 之后的激活值（范围 [%.2f, %.2f]，没有负数）：\n", static_cast<double>(range.lo),
                static_cast<double>(range.hi));
    std::printf("   对称量化（int8，-127～127）：  scale = %.5f，SQNR = %.1f dB   ← 负的那一半整数全浪费了\n",
                static_cast<double>(symScale), symDb);
    std::printf("   非对称量化（uint8，0～255）：  scale = %.5f，SQNR = %.1f dB\n\n", static_cast<double>(affine.scale),
                affineDb);
    check(affineDb > symDb + 4.0, "没有负数的数据，非对称量化应该好大约 6 dB（多一位）");
}

// ---------- 3. 校准：最大最小值还是百分位 ----------

void calibration() {
    // 大部分值在 ±3 以内，千分之一是很大的离群值
    inftut::Rng rng(2);
    std::vector<float> values;
    for (int i = 0; i < 20000; ++i) {
        float v = rng.normal();
        if (rng.uniform() < 0.001f) v *= 40.0f;
        values.push_back(v);
    }
    const quant::Range full = quant::minMax(values);
    const quant::Range clipped = quant::percentile(values, 99.9);
    const quant::QParams pFull = quant::chooseAffine(full);
    const quant::QParams pClipped = quant::chooseAffine(clipped);

    // 只看“普通”的值（绝对值 < 3）量化得怎么样：它们占 99% 以上，决定了模型的精度
    std::vector<float> typical;
    for (float v : values)
        if (std::fabs(v) < 3.0f) typical.push_back(v);
    const double fullDb = quant::sqnrDb(typical, roundTripU8(typical, pFull));
    const double clippedDb = quant::sqnrDb(typical, roundTripU8(typical, pClipped));

    std::printf("3. 带离群值的数据（%zu 个数，千分之一被放大 40 倍）：\n", values.size());
    std::printf("   最大最小值法：范围 [%7.2f, %6.2f]，scale = %.4f，普通值的 SQNR = %.1f dB\n",
                static_cast<double>(full.lo), static_cast<double>(full.hi), static_cast<double>(pFull.scale), fullDb);
    std::printf("   99.9 百分位： 范围 [%7.2f, %6.2f]，scale = %.4f，普通值的 SQNR = %.1f dB\n\n",
                static_cast<double>(clipped.lo), static_cast<double>(clipped.hi), static_cast<double>(pClipped.scale),
                clippedDb);
    check(clippedDb > fullDb + 10.0, "去掉离群值之后，普通值的精度应该明显提高");
}

// ---------- 4. 逐通道量化 ----------

void perChannel() {
    // 一个 8 行（8 个输出通道）× 64 列的权重矩阵，各行的大小相差很多。
    // 把 BatchNorm 合并进卷积之后（第 3 课），卷积核各个输出通道的大小经常是这样的。
    const int rows = 8, cols = 64;
    inftut::Rng rng(3);
    std::vector<std::vector<float>> weight(rows);
    std::vector<float> all;
    float globalMax = 0;
    for (int r = 0; r < rows; ++r) {
        const float magnitude = 0.02f * std::pow(2.0f, static_cast<float>(r));  // 0.02、0.04、…、2.56
        for (int c = 0; c < cols; ++c) {
            weight[static_cast<std::size_t>(r)].push_back(rng.normal() * magnitude);
            globalMax = std::max(globalMax, std::fabs(weight[static_cast<std::size_t>(r)].back()));
        }
        all.insert(all.end(), weight[static_cast<std::size_t>(r)].begin(), weight[static_cast<std::size_t>(r)].end());
    }
    const float tensorScale = quant::chooseSymmetricScale(globalMax);

    std::printf("4. 各行大小相差很多的权重矩阵（8 行 × 64 列）：\n");
    std::printf("   行   最大绝对值   整个矩阵一个 scale          每行一个 scale\n");
    double worstTensor = 999, worstChannel = 999;
    for (int r = 0; r < rows; ++r) {
        const std::vector<float>& row = weight[static_cast<std::size_t>(r)];
        float rowMax = 0;
        for (float v : row) rowMax = std::max(rowMax, std::fabs(v));
        const std::vector<float> byTensor = roundTripS8(row, tensorScale);
        const std::vector<float> byChannel = roundTripS8(row, quant::chooseSymmetricScale(rowMax));
        int zeros = 0;
        for (float v : byTensor) zeros += v == 0.0f;
        const double tensorDb = quant::sqnrDb(row, byTensor);
        const double channelDb = quant::sqnrDb(row, byChannel);
        worstTensor = std::min(worstTensor, tensorDb);
        worstChannel = std::min(worstChannel, channelDb);
        std::printf("   %d    %8.3f     %5.1f dB（%2d 个变成 0）      %5.1f dB\n", r, static_cast<double>(rowMax), tensorDb,
                    zeros, channelDb);
    }
    std::printf("\n");
    check(worstChannel > worstTensor + 20.0, "逐通道量化时，小的那几行不应该再被牺牲");
}

}  // namespace

int main() {
    oneNumber();
    symmetricVsAffine();
    calibration();
    perChannel();
    if (failures > 0) {
        std::printf("有 %d 项检查没有通过\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("全部检查通过\n");
    return EXIT_SUCCESS;
}
