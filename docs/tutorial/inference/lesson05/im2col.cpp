// 卷积 = im2col + 矩阵乘法。
//
// 先照着定义写一个六重循环的卷积，再用“把每个窗口摊平成一列（im2col），然后做一次矩阵乘法”的办法算一遍，
// 检查两者结果相同。这个示例只验证正确性，用很小的尺寸，带着内存检查工具运行。
//
//   macOS / Linux：clang++ -std=c++17 -I../common im2col.cpp -o im2col -pthread && ./im2col
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common im2col.cpp && im2col.exe
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "gemm.h"
#include "inftut/common.h"

namespace {

struct ConvShape {
    int inChannels, height, width;   // 输入：inChannels × height × width
    int outChannels, kernel;         // 卷积核：outChannels × inChannels × kernel × kernel
    int outHeight() const { return height - kernel + 1; }  // 步长 1、不补边
    int outWidth() const { return width - kernel + 1; }
};

// 照着定义写：对每个输出通道、每个输出位置，把窗口里的数和卷积核对应相乘再求和。
std::vector<float> convDirect(const ConvShape& s, const std::vector<float>& input, const std::vector<float>& weight) {
    const int oh = s.outHeight(), ow = s.outWidth();
    std::vector<float> output(static_cast<std::size_t>(s.outChannels) * oh * ow);
    for (int oc = 0; oc < s.outChannels; ++oc)
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                float sum = 0;
                for (int ic = 0; ic < s.inChannels; ++ic)
                    for (int ky = 0; ky < s.kernel; ++ky)
                        for (int kx = 0; kx < s.kernel; ++kx) {
                            const float in = input[(static_cast<std::size_t>(ic) * s.height + y + ky) * s.width + x + kx];
                            const float w = weight[((static_cast<std::size_t>(oc) * s.inChannels + ic) * s.kernel + ky) * s.kernel + kx];
                            sum += in * w;
                        }
                output[(static_cast<std::size_t>(oc) * oh + y) * ow + x] = sum;
            }
    return output;
}

// im2col：得到一个 (inChannels × kernel × kernel) 行、(outHeight × outWidth) 列的矩阵。
// 第 p 列是第 p 个输出位置对应的那个窗口，摊平成一列。
std::vector<float> im2col(const ConvShape& s, const std::vector<float>& input) {
    const int oh = s.outHeight(), ow = s.outWidth();
    const int rows = s.inChannels * s.kernel * s.kernel;
    const int cols = oh * ow;
    std::vector<float> columns(static_cast<std::size_t>(rows) * cols);
    for (int ic = 0; ic < s.inChannels; ++ic)
        for (int ky = 0; ky < s.kernel; ++ky)
            for (int kx = 0; kx < s.kernel; ++kx) {
                const int row = (ic * s.kernel + ky) * s.kernel + kx;
                for (int y = 0; y < oh; ++y)
                    for (int x = 0; x < ow; ++x)
                        columns[static_cast<std::size_t>(row) * cols + y * ow + x] =
                            input[(static_cast<std::size_t>(ic) * s.height + y + ky) * s.width + x + kx];
            }
    return columns;
}

// 卷积核本来就是 outChannels 行、(inChannels × kernel × kernel) 列的矩阵，不需要任何转换。
// 输出 = 卷积核矩阵 × im2col 矩阵，正好是 outChannels 行、(outHeight × outWidth) 列。
std::vector<float> convIm2col(const ConvShape& s, const std::vector<float>& input, const std::vector<float>& weight) {
    const std::vector<float> columns = im2col(s, input);
    const int M = s.outChannels;
    const int K = s.inChannels * s.kernel * s.kernel;
    const int N = s.outHeight() * s.outWidth();
    std::vector<float> output(static_cast<std::size_t>(M) * N);
    gemm::blocked(weight.data(), columns.data(), output.data(), M, K, N);
    return output;
}

}  // namespace

int main() {
    const ConvShape s{3, 20, 24, 8, 3};  // 3 通道 20×24 的输入，8 个 3×3 的卷积核
    inftut::Rng rng(7);
    std::vector<float> input(static_cast<std::size_t>(s.inChannels) * s.height * s.width);
    std::vector<float> weight(static_cast<std::size_t>(s.outChannels) * s.inChannels * s.kernel * s.kernel);
    for (float& v : input) v = rng.uniform(-1.0f, 1.0f);
    for (float& v : weight) v = rng.uniform(-1.0f, 1.0f);

    const std::vector<float> direct = convDirect(s, input, weight);
    const std::vector<float> viaGemm = convIm2col(s, input, weight);

    const int K = s.inChannels * s.kernel * s.kernel;
    const int N = s.outHeight() * s.outWidth();
    std::printf("输入 %d×%d×%d，卷积核 %d 个 %d×%d×%d，输出 %d×%d×%d\n", s.inChannels, s.height, s.width,
                s.outChannels, s.inChannels, s.kernel, s.kernel, s.outChannels, s.outHeight(), s.outWidth());
    std::printf("im2col 矩阵：%d 行 × %d 列（%d 个数，是输入的 %.1f 倍）\n", K, N, K * N,
                static_cast<double>(K) * N / static_cast<double>(input.size()));
    std::printf("矩阵乘法：(%d × %d) × (%d × %d) → %d × %d\n", s.outChannels, K, K, N, s.outChannels, N);

    const double diff = inftut::maxAbsDiff(direct, viaGemm);
    std::printf("直接卷积和 im2col + 矩阵乘法的最大误差：%.1e\n", diff);
    if (!(diff < 1e-4)) {
        std::printf("结果不一致\n");
        return EXIT_FAILURE;
    }
    std::printf("结果一致\n");
    return EXIT_SUCCESS;
}
