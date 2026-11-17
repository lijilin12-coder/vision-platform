// 张量：一块连续的 float 内存，加上四个维度（N、C、H、W）。整套教程的前四课都用它。
#pragma once

#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

namespace inftut {

struct Tensor {
    int n = 0;  // 批大小：一次送进来几张图
    int c = 0;  // 通道数
    int h = 0;  // 高
    int w = 0;  // 宽
    std::vector<float> data;  // 按 N、C、H、W 的顺序存放，长度是 n*c*h*w

    Tensor() = default;
    Tensor(int n_, int c_, int h_, int w_) { resize(n_, c_, h_, w_); }

    // 改变形状。std::vector::resize 在容量够用时不会重新申请内存（第 4 课要利用这一点）。
    // 注意：它不清零已有的元素，调用者要负责把每个元素都写一遍。
    void resize(int n_, int c_, int h_, int w_) {
        assert(n_ >= 0 && c_ >= 0 && h_ >= 0 && w_ >= 0);
        n = n_;
        c = c_;
        h = h_;
        w = w_;
        data.resize(static_cast<std::size_t>(n_) * static_cast<std::size_t>(c_) * static_cast<std::size_t>(h_) *
                    static_cast<std::size_t>(w_));
    }

    std::size_t size() const { return data.size(); }
    std::size_t bytes() const { return data.size() * sizeof(float); }

    // 第 in 张图、第 ic 个通道、第 y 行、第 x 列的元素在 data 里的下标
    std::size_t index(int in, int ic, int y, int x) const {
        assert(in >= 0 && in < n && ic >= 0 && ic < c && y >= 0 && y < h && x >= 0 && x < w);
        return ((static_cast<std::size_t>(in) * static_cast<std::size_t>(c) + static_cast<std::size_t>(ic)) *
                    static_cast<std::size_t>(h) +
                static_cast<std::size_t>(y)) *
                   static_cast<std::size_t>(w) +
               static_cast<std::size_t>(x);
    }
    float& at(int in, int ic, int y, int x) { return data[index(in, ic, y, x)]; }
    float at(int in, int ic, int y, int x) const { return data[index(in, ic, y, x)]; }

    bool sameShape(const Tensor& other) const { return n == other.n && c == other.c && h == other.h && w == other.w; }

    std::string shapeText() const {
        return std::to_string(n) + "x" + std::to_string(c) + "x" + std::to_string(h) + "x" + std::to_string(w);
    }
};

}  // namespace inftut
