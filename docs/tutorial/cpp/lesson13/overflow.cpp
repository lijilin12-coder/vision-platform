// 越界写：循环多走了一步。【故意】有错，用来观察内存检查工具的报告。
#include <cstddef>
#include <iostream>
#include <vector>

// 把一行像素复制到目标缓冲区。
void copyRow(const std::vector<unsigned char>& row, unsigned char* dest) {
    for (std::size_t i = 0; i <= row.size(); ++i) {  // ← 应该是 <，写成了 <=
        dest[i] = row[i];
    }
}

int main() {
    const std::vector<unsigned char> row(640, 128);
    std::vector<unsigned char> buffer(640);
    copyRow(row, buffer.data());  // 多读了 1 个字节，也多写了 1 个字节
    std::cout << "复制完成，最后一个像素 = " << static_cast<int>(buffer[639]) << "\n";
    return 0;
}
