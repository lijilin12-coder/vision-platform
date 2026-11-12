// 自己写一个字符串类，用来理解“类到底解决了什么问题”。
// 实际项目里请直接用 std::string，不要自己写。
#pragma once

#include <cstddef>
#include <iosfwd>

namespace lesson01 {

class MyString {
public:
    // ---- 构造：对象一出生就是合法的 ----
    MyString() noexcept;                  // 空字符串（不申请内存）
    explicit MyString(const char* text);  // explicit：禁止把 const char* 悄悄转换成 MyString

    // ---- 析构：对象消失时自动归还内存 ----
    ~MyString();

    // ---- 拷贝：复制出一份独立的内容 ----
    MyString(const MyString& other);
    MyString& operator=(const MyString& other);

    // ---- 移动：把内容“搬”过来，不复制 ----
    MyString(MyString&& other) noexcept;
    MyString& operator=(MyString&& other) noexcept;

    // ---- 只读操作：都标了 const，保证不会修改对象 ----
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    const char* c_str() const { return data_ ? data_ : ""; }
    char at(std::size_t index) const;  // 越界时抛出 std::out_of_range

    // ---- 修改操作：每个操作结束时，不变量仍然成立 ----
    void append(const MyString& other);

    // ---- 运算符重载：让自己的类型用起来像内置类型 ----
    friend bool operator==(const MyString& a, const MyString& b);
    friend bool operator!=(const MyString& a, const MyString& b) { return !(a == b); }
    friend MyString operator+(const MyString& a, const MyString& b);
    friend std::ostream& operator<<(std::ostream& out, const MyString& s);

private:
    void swap(MyString& other) noexcept;

    // 不变量（任何时刻都成立）：
    //   要么是空字符串：data_ == nullptr 且 size_ == 0
    //   要么 data_ 指向一块自己独占的、长度为 size_ + 1 的内存，并且 data_[size_] == '\0'
    std::size_t size_;
    char* data_;
};

}  // namespace lesson01
