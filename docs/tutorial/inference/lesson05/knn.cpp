// 最近邻搜索也是矩阵乘法。
//
// PatchCore 一类的异常检测方法：把正常样本的特征存成一个“记忆库”，检测时对图像上每个位置的特征，
// 在记忆库里找最近的一个，距离越大越可疑。
//
//   Q 个查询特征、N 个记忆库特征、每个 D 维。要算 Q × N 个距离。
//   ‖q − m‖² = ‖q‖² + ‖m‖² − 2·q·m      其中 q·m 对所有 (q, m) 一起算，就是矩阵乘法 Q × Mᵀ
//
//   macOS / Linux：clang++ -std=c++17 -O2 -march=native -I../common knn.cpp -o knn -pthread && ./knn
//   Windows：      cl /std:c++17 /O2 /arch:AVX2 /EHsc /utf-8 /I..\common knn.cpp && knn.exe
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

#include "gemm.h"
#include "inftut/common.h"

namespace {

struct Nearest {
    std::vector<int> index;        // 每个查询在记忆库里最近的那一个
    std::vector<float> distance2;  // 距离的平方
};

// 直接写：对每一对 (查询, 记忆库特征) 逐维相减、平方、求和。
Nearest searchDirect(const std::vector<float>& queries, const std::vector<float>& memory, int Q, int N, int D) {
    Nearest result{std::vector<int>(static_cast<std::size_t>(Q)), std::vector<float>(static_cast<std::size_t>(Q))};
    for (int q = 0; q < Q; ++q) {
        float best = std::numeric_limits<float>::max();
        int bestIndex = -1;
        for (int m = 0; m < N; ++m) {
            float sum = 0;
            for (int d = 0; d < D; ++d) {
                const float diff = queries[static_cast<std::size_t>(q) * D + d] - memory[static_cast<std::size_t>(m) * D + d];
                sum += diff * diff;
            }
            if (sum < best) {
                best = sum;
                bestIndex = m;
            }
        }
        result.index[static_cast<std::size_t>(q)] = bestIndex;
        result.distance2[static_cast<std::size_t>(q)] = best;
    }
    return result;
}

std::vector<float> squaredNorms(const std::vector<float>& rows, int count, int D) {
    std::vector<float> norms(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        float sum = 0;
        for (int d = 0; d < D; ++d) sum += rows[static_cast<std::size_t>(i) * D + d] * rows[static_cast<std::size_t>(i) * D + d];
        norms[static_cast<std::size_t>(i)] = sum;
    }
    return norms;
}

// 用矩阵乘法：先算出全部内积，再加上两边的模长平方。
// memoryT 是记忆库的转置（D 行 N 列），memoryNorms 是每个记忆库特征的模长平方。
// 这两样只和记忆库有关，方案上线时算一次，以后每张图都直接用。
Nearest searchGemm(const std::vector<float>& queries, const std::vector<float>& memoryT,
                   const std::vector<float>& memoryNorms, int Q, int N, int D, int threads) {
    std::vector<float> dots(static_cast<std::size_t>(Q) * N);
    gemm::threaded(queries.data(), memoryT.data(), dots.data(), Q, D, N, threads);  // (Q×D) × (D×N) → Q×N

    const std::vector<float> queryNorms = squaredNorms(queries, Q, D);
    Nearest result{std::vector<int>(static_cast<std::size_t>(Q)), std::vector<float>(static_cast<std::size_t>(Q))};
    for (int q = 0; q < Q; ++q) {
        float best = std::numeric_limits<float>::max();
        int bestIndex = -1;
        const float* row = dots.data() + static_cast<std::size_t>(q) * N;
        for (int m = 0; m < N; ++m) {
            const float dist = memoryNorms[static_cast<std::size_t>(m)] - 2.0f * row[m];  // ‖q‖² 对同一个 q 是常数，最后再加
            if (dist < best) {
                best = dist;
                bestIndex = m;
            }
        }
        result.index[static_cast<std::size_t>(q)] = bestIndex;
        result.distance2[static_cast<std::size_t>(q)] = best + queryNorms[static_cast<std::size_t>(q)];
    }
    return result;
}

}  // namespace

int main() {
    const int Q = 784;    // 一张图 28 × 28 个位置
    const int N = 4096;   // 记忆库里 4096 个特征
    const int D = 256;    // 每个特征 256 维
    inftut::Rng rng(11);
    std::vector<float> queries(static_cast<std::size_t>(Q) * D);
    std::vector<float> memory(static_cast<std::size_t>(N) * D);
    for (float& v : queries) v = rng.normal();
    for (float& v : memory) v = rng.normal();

    // 记忆库的转置和模长：只算一次
    std::vector<float> memoryT(static_cast<std::size_t>(D) * N);
    for (int m = 0; m < N; ++m)
        for (int d = 0; d < D; ++d) memoryT[static_cast<std::size_t>(d) * N + m] = memory[static_cast<std::size_t>(m) * D + d];
    const std::vector<float> memoryNorms = squaredNorms(memory, N, D);
    const int threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    Nearest direct, one, many;
    const double directMs = inftut::medianMs(3, [&] { direct = searchDirect(queries, memory, Q, N, D); });
    const double oneMs = inftut::medianMs(3, [&] { one = searchGemm(queries, memoryT, memoryNorms, Q, N, D, 1); });
    const double manyMs = inftut::medianMs(3, [&] { many = searchGemm(queries, memoryT, memoryNorms, Q, N, D, threads); });

    std::printf("%d 个查询 × %d 个记忆库特征，每个 %d 维（记忆库 %.1f MB），取 3 次的中位数：\n", Q, N, D,
                static_cast<double>(memory.size()) * 4 / 1048576.0);
    std::printf("  逐对相减求平方和：        %7.1f ms\n", directMs);
    std::printf("  矩阵乘法（1 个线程）：    %7.1f ms，快 %.1f 倍\n", oneMs, directMs / oneMs);
    std::printf("  矩阵乘法（%d 个线程）：    %7.1f ms，快 %.1f 倍\n", threads, manyMs, directMs / manyMs);

    // 两种算法的浮点误差不同：比较“找到的是不是同一个”和“距离差多少”
    int sameIndex = 0;
    for (int q = 0; q < Q; ++q) sameIndex += direct.index[static_cast<std::size_t>(q)] == many.index[static_cast<std::size_t>(q)];
    const double diff = inftut::maxAbsDiff(direct.distance2, many.distance2);
    std::printf("最近邻相同的查询：%d / %d；距离平方的最大误差 %.1e（距离平方本身在 %.0f 左右）\n", sameIndex, Q, diff,
                static_cast<double>(direct.distance2[0]));
    if (sameIndex < Q * 99 / 100 || !(diff < 0.05)) {
        std::printf("结果不一致\n");
        return EXIT_FAILURE;
    }
    std::printf("结果一致\n");
    return EXIT_SUCCESS;
}
