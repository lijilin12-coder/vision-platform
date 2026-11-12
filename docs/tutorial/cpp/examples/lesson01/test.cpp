#include <cassert>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "my_string.h"

using lesson01::MyString;

MyString makeGreeting() {
    MyString s("hello");
    s.append(MyString(", blob"));
    return s;  // 返回局部对象：不会复制内容
}

int main() {
    // 构造：一出生就合法
    MyString empty;
    assert(empty.size() == 0 && empty.empty());
    assert(empty == MyString(""));

    MyString a("blob");
    assert(a.size() == 4);
    assert(a.at(0) == 'b');

    // 拷贝：两份独立的内容，改一个不影响另一个
    MyString b = a;
    b.append(MyString("s"));
    assert(a == MyString("blob"));
    assert(b == MyString("blobs"));

    // 拷贝赋值，包括自己赋值给自己
    MyString c;
    c = a;
    assert(c == a);
    MyString& self = c;
    c = self;
    assert(c == MyString("blob"));

    // 移动：内容被搬走，原对象变成合法的空字符串
    MyString d = std::move(b);
    assert(d == MyString("blobs"));
    assert(b.empty());           // 被移走后仍然可以安全使用
    b = MyString("again");       // 也可以重新赋值
    assert(b == MyString("again"));

    // 运算符
    assert(MyString("ab") + MyString("cd") == MyString("abcd"));
    assert(a != d);
    std::ostringstream out;
    out << makeGreeting();
    assert(out.str() == "hello, blob");

    // 越界访问会被发现，而不是悄悄读到别的内存
    bool caught = false;
    try {
        (void)a.at(99);
    } catch (const std::out_of_range&) {
        caught = true;
    }
    assert(caught);

    // 放进容器：vector 扩容时会移动元素
    std::vector<MyString> names;
    for (int i = 0; i < 100; ++i) names.push_back(MyString("name"));
    assert(names.size() == 100 && names[99] == MyString("name"));

    std::cout << "全部通过\n";
    return 0;
    // 这里所有 MyString 自动析构，没有任何一处手动释放内存
}
