#include "my_string.h"

#include <cstring>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace lesson01 {

// ---- 构造 ----

MyString::MyString() noexcept : size_(0), data_(nullptr) {}

MyString::MyString(const char* text) : size_(std::strlen(text)), data_(nullptr) {
    if (size_ == 0) return;
    data_ = new char[size_ + 1];
    std::memcpy(data_, text, size_ + 1);
}

// ---- 析构 ----

MyString::~MyString() { delete[] data_; }  // delete[] 空指针是安全的，什么也不做

// ---- 拷贝 ----

// 拷贝构造：申请一块新内存，把内容复制过去。之后两个对象互不影响。
MyString::MyString(const MyString& other) : size_(other.size_), data_(nullptr) {
    if (size_ == 0) return;
    data_ = new char[size_ + 1];
    std::memcpy(data_, other.data_, size_ + 1);
}

// 拷贝赋值：先复制出一个临时对象，再和它交换内容。
// 临时对象离开作用域时，会带着“我原来的内存”一起析构。
// 这样写的好处：a = a 不会出错；申请内存失败时，自己的内容保持不变。
MyString& MyString::operator=(const MyString& other) {
    MyString copy(other);
    swap(copy);
    return *this;
}

// ---- 移动 ----

// 移动构造：直接接管对方的内存，然后让对方变成空字符串。
// 被移走之后，对方仍然满足不变量（是一个合法的空字符串），可以安全地析构或重新赋值。
MyString::MyString(MyString&& other) noexcept : size_(other.size_), data_(other.data_) {
    other.size_ = 0;
    other.data_ = nullptr;
}

MyString& MyString::operator=(MyString&& other) noexcept {
    swap(other);  // 交换之后，我原来的内存由 other 在析构时释放
    return *this;
}

void MyString::swap(MyString& other) noexcept {
    std::swap(size_, other.size_);
    std::swap(data_, other.data_);
}

// ---- 只读操作 ----

char MyString::at(std::size_t index) const {
    if (index >= size_) throw std::out_of_range("MyString::at 下标越界");
    return data_[index];
}

// ---- 修改操作 ----

void MyString::append(const MyString& other) {
    if (other.empty()) return;
    const std::size_t newSize = size_ + other.size_;
    char* buffer = new char[newSize + 1];
    std::memcpy(buffer, c_str(), size_);
    std::memcpy(buffer + size_, other.data_, other.size_ + 1);  // 连同结尾的 '\0'
    delete[] data_;
    data_ = buffer;
    size_ = newSize;
}

// ---- 运算符 ----

bool operator==(const MyString& a, const MyString& b) {
    return a.size_ == b.size_ && std::memcmp(a.c_str(), b.c_str(), a.size_) == 0;
}

MyString operator+(const MyString& a, const MyString& b) {
    MyString result(a);
    result.append(b);
    return result;
}

std::ostream& operator<<(std::ostream& out, const MyString& s) { return out << s.c_str(); }

}  // namespace lesson01
