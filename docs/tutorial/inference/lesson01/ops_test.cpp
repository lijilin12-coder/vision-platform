// 用能手算的小例子检查每个算子。全部通过时输出 “全部通过”，返回 0。
//
//   macOS / Linux：clang++ -std=c++17 -I../common ops_test.cpp -o ops_test && ./ops_test
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common ops_test.cpp && ops_test.exe
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "ops.h"

using inftut::Tensor;

namespace {

int failures = 0;

void expectNear(const std::string& what, const std::vector<float>& actual, const std::vector<float>& expected) {
    bool ok = actual.size() == expected.size();
    for (std::size_t i = 0; ok && i < actual.size(); ++i) ok = std::fabs(actual[i] - expected[i]) < 1e-5f;
    std::cout << (ok ? "  通过  " : "  失败  ") << what << "\n";
    if (!ok) {
        ++failures;
        std::cout << "        实际：";
        for (float v : actual) std::cout << v << " ";
        std::cout << "\n        期望：";
        for (float v : expected) std::cout << v << " ";
        std::cout << "\n";
    }
}

Tensor make(int n, int c, int h, int w, std::vector<float> values) {
    Tensor t(n, c, h, w);
    t.data = std::move(values);
    return t;
}

}  // namespace

int main() {
    // 3×3 的输入：
    //   1 2 3
    //   4 5 6
    //   7 8 9
    const Tensor x = make(1, 1, 3, 3, {1, 2, 3, 4, 5, 6, 7, 8, 9});

    // 2×2 的卷积核 [1 0; 0 1]：每个输出 = 左上角 + 右下角
    const Tensor diag = make(1, 1, 2, 2, {1, 0, 0, 1});
    const Tensor zeroBias = make(1, 1, 1, 1, {0});
    expectNear("卷积：2×2 的核，不补零", inftut::conv2d(x, diag, zeroBias, 1, 0).data, {6, 8, 12, 14});

    // 3×3 的全 1 卷积核，补一圈零：每个输出 = 周围 3×3 的和（边上的缺一部分）
    const Tensor ones = make(1, 1, 3, 3, {1, 1, 1, 1, 1, 1, 1, 1, 1});
    expectNear("卷积：3×3 的核，补一圈零，输出和输入一样大", inftut::conv2d(x, ones, zeroBias, 1, 1).data,
               {12, 21, 16, 27, 45, 33, 24, 39, 28});

    // 步长 2：只算第 0、2 行和第 0、2 列
    expectNear("卷积：步长 2", inftut::conv2d(x, ones, zeroBias, 2, 1).data, {12, 16, 24, 28});

    // 偏置：每个输出加 10
    expectNear("卷积：偏置", inftut::conv2d(x, diag, make(1, 1, 1, 1, {10}), 1, 0).data, {16, 18, 22, 24});

    // 两个输入通道、两个输出通道的 1×1 卷积：输出通道 0 = 通道 0 + 通道 1，输出通道 1 = 通道 0 - 通道 1
    const Tensor twoCh = make(1, 2, 1, 2, {1, 2, 10, 20});
    const Tensor mix = make(2, 2, 1, 1, {1, 1, 1, -1});
    expectNear("卷积：多个通道", inftut::conv2d(twoCh, mix, make(1, 2, 1, 1, {0, 0}), 1, 0).data, {11, 22, -9, -18});

    expectNear("ReLU", inftut::relu(make(1, 1, 1, 4, {-2, -0.5f, 0, 3})).data, {0, 0, 0, 3});

    const Tensor x4 = make(1, 1, 4, 4, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    expectNear("最大池化 2×2", inftut::maxPool(x4, 2).data, {6, 8, 14, 16});

    // 全连接：输入 (1,2,3)，两个输出：1*1+2*2+3*3+0.5 = 14.5；1-2+3 = 2
    const Tensor v = make(1, 3, 1, 1, {1, 2, 3});
    const Tensor fcW = make(2, 3, 1, 1, {1, 2, 3, 1, -1, 1});
    expectNear("全连接", inftut::linear(v, fcW, make(1, 2, 1, 1, {0.5f, 0})).data, {14.5f, 2});

    // softmax：三个相同的数 → 各 1/3；数值很大时也不能溢出
    expectNear("softmax：相同的输入", inftut::softmax(make(1, 3, 1, 1, {2, 2, 2})).data,
               {1.0f / 3, 1.0f / 3, 1.0f / 3});
    const float e = std::exp(1.0f);
    expectNear("softmax：很大的输入不溢出", inftut::softmax(make(1, 2, 1, 1, {1000, 1001})).data,
               {1 / (1 + e), e / (1 + e)});

    expectNear("逐元素相加", inftut::add(make(1, 1, 1, 3, {1, 2, 3}), make(1, 1, 1, 3, {10, 20, 30})).data,
               {11, 22, 33});

    // 批归一化：通道 0 用 gamma=2, beta=1, mean=3, var=4 → y = 2*(x-3)/2 + 1 = x - 2（忽略很小的 eps）
    const Tensor bn = inftut::batchNorm(make(1, 1, 1, 3, {3, 5, 7}), make(1, 1, 1, 1, {2}), make(1, 1, 1, 1, {1}),
                                        make(1, 1, 1, 1, {3}), make(1, 1, 1, 1, {4}));
    expectNear("批归一化", bn.data, {1, 3, 5});

    // 原地版本：输入和输出是同一个张量
    Tensor inplace = make(1, 1, 1, 3, {-1, 2, -3});
    inftut::reluInto(inplace, inplace);
    expectNear("ReLU 原地", inplace.data, {0, 2, 0});

    std::cout << (failures == 0 ? "全部通过\n" : "有失败的检查\n");
    return failures == 0 ? 0 : 1;
}
