// 演示：只写了构造函数和析构函数、没有写拷贝的类，一复制就出错。
// 编译运行：clang++ -std=c++17 -g -fsanitize=address broken_copy.cpp -o broken_copy && ./broken_copy
#include <cstdio>
#include <cstring>

class BrokenString {
public:
    explicit BrokenString(const char* text) : size_(std::strlen(text)), data_(new char[size_ + 1]) {
        std::memcpy(data_, text, size_ + 1);
    }
    ~BrokenString() { delete[] data_; }

    const char* c_str() const { return data_; }

private:
    std::size_t size_;
    char* data_;
};

int main() {
    BrokenString a("blob");
    BrokenString b = a;  // 编译器自动生成的拷贝：只复制了指针，a 和 b 指向同一块内存
    std::printf("%s %s\n", a.c_str(), b.c_str());
    return 0;
    // main 结束时，b 先析构，释放了内存；然后 a 析构，再次释放同一块内存 → 重复释放
}
