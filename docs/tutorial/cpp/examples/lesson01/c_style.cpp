// 第 1 步之前：不用类，只用结构体和函数来表示字符串。
// 这段代码能运行，但请留意注释里标出的 4 个隐患。
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Str {
    char* data;        // 指向一块堆内存
    std::size_t size;  // 字符个数（不含结尾的 '\0'）
};

Str strCreate(const char* text) {
    Str s;
    s.size = std::strlen(text);
    s.data = static_cast<char*>(std::malloc(s.size + 1));
    std::memcpy(s.data, text, s.size + 1);
    return s;
}

void strDestroy(Str& s) {
    std::free(s.data);
    s.data = nullptr;
    s.size = 0;
}

int main() {
    Str name = strCreate("blob");
    std::printf("%s 的长度是 %zu\n", name.data, name.size);

    // 隐患 1：任何人都能直接改成员，数据立刻自相矛盾
    // name.size = 100;            // size 说有 100 个字符，实际只有 4 个

    // 隐患 2：忘记初始化
    // Str other;                  // data 是随机地址，一用就出错

    // 隐患 3：“复制”只复制了指针，两个变量指向同一块内存
    // Str copy = name;
    // strDestroy(copy);
    // strDestroy(name);           // 同一块内存被释放两次

    // 隐患 4：忘记调用 strDestroy，内存泄漏
    strDestroy(name);
    return 0;
}
