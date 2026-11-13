// 数据竞争：两个线程同时修改同一个变量。【故意】有错，用来观察结果和线程检查工具的报告。
// macOS / Linux：clang++ -std=c++17 -g -fsanitize=thread race.cpp -o race && ./race
#include <iostream>
#include <thread>

int counter = 0;  // 两个线程共享

void addMany() {
    for (int i = 0; i < 1000000; ++i) {
        ++counter;  // 看起来是一步，实际是三步：读出来、加一、写回去
    }
}

int main() {
    std::thread a(addMany);
    std::thread b(addMany);
    a.join();
    b.join();
    std::cout << "期望 2000000，实际 " << counter << "\n";
    return 0;
}
