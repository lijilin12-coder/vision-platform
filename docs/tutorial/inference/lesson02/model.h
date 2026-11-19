// 第 2～4 课共用的示例网络：一个带“跳跃连接”的小卷积网络（ResNet 的最小形态）。
//
//   input ─ conv1 ─ bn1 ─ relu1 ─ pool1 ─┬─ conv2a ─ bn2a ─ relu2a ─ conv2b ─ bn2b ─┐
//                                        │                                          add2 ─ relu2 ─ pool2 ─ ...
//                                        └──────────────────────────────────────────┘
//   ... ─ conv3 ─ bn3 ─ relu3 ─ pool3 ─ fc ─ prob（Softmax）
//
// pool1 的输出被两个节点使用（conv2a 和 add2），所以它不是一条链，而是一张图。
#pragma once

#include <cmath>
#include <string>

#include "graph.h"
#include "inftut/common.h"

namespace inftut {

// 给 Conv 后面的 BatchNorm 生成一组参数。
inline void addBatchNorm(Graph& g, Rng& rng, const std::string& name, const std::string& in, int channels) {
    Tensor gamma(1, channels, 1, 1), beta(1, channels, 1, 1), mean(1, channels, 1, 1), var(1, channels, 1, 1);
    for (int c = 0; c < channels; ++c) {
        const std::size_t i = static_cast<std::size_t>(c);
        gamma.data[i] = 1.0f + 0.2f * rng.normal();
        beta.data[i] = 0.1f * rng.normal();
        mean.data[i] = 0.1f * rng.normal();
        var.data[i] = rng.uniform(0.5f, 1.5f);
    }
    g.nodes.push_back(batchNormNode(name, in, std::move(gamma), std::move(beta), std::move(mean), std::move(var)));
}

inline void addConv(Graph& g, Rng& rng, const std::string& name, const std::string& in, int inC, int outC) {
    const float stddev = std::sqrt(2.0f / static_cast<float>(inC * 9));
    Tensor bias(1, outC, 1, 1);
    for (float& b : bias.data) b = 0.05f * rng.normal();
    g.nodes.push_back(convNode(name, in, randomTensor(rng, outC, inC, 3, 3, stddev), std::move(bias), 1, 1));
}

// size：输入图像的边长（必须是 8 的倍数）；channels：第一层卷积的输出通道数。
inline Graph buildSmallResNet(int size = 32, int channels = 8, std::uint64_t seed = 1) {
    Rng rng(seed);
    const int c = channels;
    Graph g;
    g.nodes.push_back(inputNode("input", Shape{1, 3, size, size}));

    addConv(g, rng, "conv1", "input", 3, c);
    addBatchNorm(g, rng, "bn1", "conv1", c);
    g.nodes.push_back(simpleNode("relu1", OpType::Relu, {"bn1"}));
    g.nodes.push_back(maxPoolNode("pool1", "relu1", 2));

    // 残差块：两层卷积的结果，加上这个块的输入
    addConv(g, rng, "conv2a", "pool1", c, c);
    addBatchNorm(g, rng, "bn2a", "conv2a", c);
    g.nodes.push_back(simpleNode("relu2a", OpType::Relu, {"bn2a"}));
    addConv(g, rng, "conv2b", "relu2a", c, c);
    addBatchNorm(g, rng, "bn2b", "conv2b", c);
    g.nodes.push_back(simpleNode("add2", OpType::Add, {"bn2b", "pool1"}));
    g.nodes.push_back(simpleNode("relu2", OpType::Relu, {"add2"}));
    g.nodes.push_back(maxPoolNode("pool2", "relu2", 2));

    addConv(g, rng, "conv3", "pool2", c, 2 * c);
    addBatchNorm(g, rng, "bn3", "conv3", 2 * c);
    g.nodes.push_back(simpleNode("relu3", OpType::Relu, {"bn3"}));
    g.nodes.push_back(maxPoolNode("pool3", "relu3", 2));

    const int features = 2 * c * (size / 8) * (size / 8);
    g.nodes.push_back(linearNode("fc", "pool3", randomTensor(rng, 10, features, 1, 1, std::sqrt(1.0f / static_cast<float>(features))),
                                 Tensor(1, 10, 1, 1)));
    g.nodes.push_back(simpleNode("prob", OpType::Softmax, {"fc"}));
    g.output = "prob";
    return g;
}

// 一张随机的输入“图像”，像素值在 0～1 之间。
inline Tensor randomImage(int size, std::uint64_t seed = 2) {
    Rng rng(seed);
    Tensor image(1, 3, size, size);
    for (float& v : image.data) v = rng.uniform();
    return image;
}

}  // namespace inftut
