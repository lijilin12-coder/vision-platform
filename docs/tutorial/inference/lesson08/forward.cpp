// 第 8 课示例一：走一遍 GPT 的前向计算，打印每一步的形状；验证因果性；贪心生成。
//
//   macOS / Linux：clang++ -std=c++17 -I../common forward.cpp -o forward && ./forward
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common forward.cpp && forward.exe
#include <iostream>
#include <vector>

#include "tiny_gpt.h"

using namespace tinygpt;

namespace {

void printShape(const char* name, const Mat& m) { std::cout << "  " << name << "：" << m.rows << " × " << m.cols << "\n"; }

void printTokens(const std::vector<int>& tokens) {
    for (int t : tokens) std::cout << t << " ";
    std::cout << "\n";
}

}  // namespace

int main() {
    Config cfg;  // 词表 64，隐藏维度 32，4 个头，2 层
    const Model model = makeModel(cfg, 42);
    std::cout << "模型：词表 " << cfg.vocab << "，隐藏维度 " << cfg.dim << "，" << cfg.heads << " 个头，" << cfg.layers
              << " 层，共 " << parameterCount(model) << " 个参数\n\n";

    const std::vector<int> prompt = {5, 17, 3, 42, 8, 21};
    std::cout << "输入 " << prompt.size() << " 个 token：";
    printTokens(prompt);

    // ---- 1. 一步一步走，看形状 ----
    std::cout << "\n每一步的形状（行数 = token 数）：\n";
    Mat x = embed(model, prompt);
    printShape("embedding 之后       ", x);
    for (int l = 0; l < cfg.layers; ++l) {
        const Layer& layer = model.layers[static_cast<std::size_t>(l)];
        const Mat attn = selfAttention(layer, cfg, layerNorm(x, layer.norm1));
        addInPlace(x, attn);
        const Mat ff = mlp(layer, layerNorm(x, layer.norm2));
        addInPlace(x, ff);
        std::cout << "  第 " << l << " 层之后          ：" << x.rows << " × " << x.cols << "\n";
    }
    const Mat logits = toLogits(model, x);
    printShape("logits              ", logits);

    // 分步算的结果应该和 forward() 完全一样
    const Mat reference = forward(model, prompt);
    if (inftut::maxAbsDiff(logits.v, reference.v) > 1e-4) {
        std::cout << "错误：分步计算和 forward() 的结果不一致\n";
        return 1;
    }

    // ---- 2. 最后一行 logits → 概率 → 下一个 token ----
    std::vector<float> probs(logits.row(logits.rows - 1), logits.row(logits.rows - 1) + cfg.vocab);
    softmaxInPlace(probs.data(), cfg.vocab);
    const int next = argmax(probs.data(), cfg.vocab);
    std::cout << "\n最后一行 logits 变成概率后，最大的是 token " << next << "，概率 " << probs[static_cast<std::size_t>(next)] << "\n";

    // ---- 3. 因果性：改掉后面的 token，前面位置的输出不变 ----
    std::vector<int> changed = prompt;
    changed.back() = 60;  // 只改最后一个 token
    const Mat logitsChanged = forward(model, changed);
    double earlierDiff = 0;
    double lastDiff = 0;
    for (int r = 0; r < logits.rows; ++r) {
        for (int c = 0; c < logits.cols; ++c) {
            const double d = std::fabs(static_cast<double>(logits.row(r)[c]) - static_cast<double>(logitsChanged.row(r)[c]));
            if (r + 1 < logits.rows) earlierDiff = std::max(earlierDiff, d);
            else lastDiff = std::max(lastDiff, d);
        }
    }
    std::cout << "\n把最后一个 token 从 " << prompt.back() << " 改成 60：\n";
    std::cout << "  前 " << logits.rows - 1 << " 行 logits 的最大变化：" << earlierDiff << "\n";
    std::cout << "  最后一行 logits 的最大变化：" << lastDiff << "\n";
    if (earlierDiff > 1e-4 || lastDiff < 1e-2) {
        std::cout << "错误：因果性不成立\n";
        return 1;
    }

    // ---- 4. 贪心生成 ----
    const std::vector<int> generated = generateGreedy(model, prompt, 12);
    std::cout << "\n贪心生成 12 个 token：";
    printTokens(generated);
    if (generated[prompt.size()] != next) {
        std::cout << "错误：生成的第一个 token 应该是 " << next << "\n";
        return 1;
    }
    return 0;
}
