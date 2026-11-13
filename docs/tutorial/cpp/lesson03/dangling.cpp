// 悬空引用：引用指向的东西已经不在了。这个程序【故意】有错，用来观察内存检查工具的报告。
// macOS / Linux：clang++ -std=c++17 -g -fsanitize=address dangling.cpp -o dangling && ./dangling
// Windows：      cl /std:c++17 /EHsc /utf-8 /Zi /fsanitize=address dangling.cpp && dangling.exe
#include <iostream>
#include <vector>

int main() {
    std::vector<int> areas;
    areas.push_back(42);

    const int& first = areas[0];  // first 是 areas[0] 的另一个名字

    // 继续添加元素。vector 的空间不够时会申请一块更大的内存，
    // 把旧元素搬过去，然后释放旧内存。first 仍然指着那块已经释放的旧内存。
    for (int i = 0; i < 100; ++i) areas.push_back(i);

    std::cout << first << "\n";  // ← 读取已经释放的内存
    return 0;
}
