// 未定义行为：有符号整数溢出。【故意】有错，用来观察 UBSan 的报告。
#include <cstdlib>
#include <iostream>

// 计算一张图像需要多少字节。
int imageBytes(int width, int height, int channels) {
    return width * height * channels;  // ← int 最大约 21 亿，大图会溢出
}

// 修复：先转换成足够大的无符号类型再相乘。
std::size_t imageBytesFixed(int width, int height, int channels) {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels);
}

int main(int argc, char**) {
    // 用 argc 参与计算，防止编译器在编译期就把结果算出来
    const int width = 30000 + argc - 1;
    const int height = 30000;

    std::cout << "正确的字节数：" << imageBytesFixed(width, height, 3) << "\n";
    std::cout << "用 int 计算： " << imageBytes(width, height, 3) << "\n";
    return 0;
}
