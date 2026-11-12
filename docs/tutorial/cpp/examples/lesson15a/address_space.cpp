// 线程共享什么、不共享什么：把变量的地址打印出来看。
//
// 同一个进程里的线程共用一个地址空间：全局变量、堆上的对象，在所有线程里是同一个地址、同一份数据。
// 每个线程有自己的栈：局部变量各有一份，地址也不同。
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

int globalCounter = 0;  // 全局变量：在“数据区”，所有线程共享

struct Report {
    std::string name;
    std::uintptr_t globalAddress = 0;  // 全局变量的地址
    std::uintptr_t heapAddress = 0;    // 堆上对象的地址
    std::uintptr_t localAddress = 0;   // 这个线程里局部变量的地址
    std::uintptr_t codeAddress = 0;    // 函数的地址
};

std::uintptr_t addressOf(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

// 每个线程都执行这个函数。sharedOnHeap 指向同一个堆对象。
void work(const std::string& name, const std::shared_ptr<int>& sharedOnHeap, Report& report) {
    int local = 0;  // 局部变量：在这个线程自己的栈上
    report.name = name;
    report.globalAddress = addressOf(&globalCounter);
    report.heapAddress = addressOf(sharedOnHeap.get());
    report.localAddress = addressOf(&local);
    report.codeAddress = addressOf(reinterpret_cast<const void*>(&addressOf));
    (void)local;
}

std::string hex(std::uintptr_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

int main() {
    std::cout << "这台机器可以同时运行 " << std::thread::hardware_concurrency() << " 个线程（逻辑核心数）\n\n";

    const auto sharedOnHeap = std::make_shared<int>(42);  // 堆上的对象
    Report mainReport, aReport, bReport;

    work("主线程", sharedOnHeap, mainReport);
    std::thread a(work, "线程 A", std::cref(sharedOnHeap), std::ref(aReport));
    std::thread b(work, "线程 B", std::cref(sharedOnHeap), std::ref(bReport));
    a.join();
    b.join();

    for (const Report* r : {&mainReport, &aReport, &bReport}) {
        std::cout << r->name << "\n"
                  << "  代码（函数）  " << hex(r->codeAddress) << "\n"
                  << "  全局变量      " << hex(r->globalAddress) << "\n"
                  << "  堆上的对象    " << hex(r->heapAddress) << "\n"
                  << "  局部变量（栈）" << hex(r->localAddress) << "\n";
    }

    const bool sameCode = mainReport.codeAddress == aReport.codeAddress && aReport.codeAddress == bReport.codeAddress;
    const bool sameGlobal = mainReport.globalAddress == aReport.globalAddress && aReport.globalAddress == bReport.globalAddress;
    const bool sameHeap = mainReport.heapAddress == aReport.heapAddress && aReport.heapAddress == bReport.heapAddress;
    const bool differentStacks = mainReport.localAddress != aReport.localAddress &&
                                 aReport.localAddress != bReport.localAddress &&
                                 mainReport.localAddress != bReport.localAddress;

    std::cout << "\n代码的地址相同：    " << (sameCode ? "是" : "否") << "（所有线程执行同一份代码）\n"
              << "全局变量的地址相同：" << (sameGlobal ? "是" : "否") << "（共享）\n"
              << "堆上对象的地址相同：" << (sameHeap ? "是" : "否") << "（共享）\n"
              << "局部变量的地址不同：" << (differentStacks ? "是" : "否") << "（每个线程有自己的栈）\n";

    // 两个线程的栈相距多远：操作系统给每个线程单独分配了一块栈空间
    const auto distance = aReport.localAddress > bReport.localAddress ? aReport.localAddress - bReport.localAddress
                                                                       : bReport.localAddress - aReport.localAddress;
    std::cout << "线程 A 和线程 B 的栈相距约 " << distance / 1024 << " KB\n";

    return sameCode && sameGlobal && sameHeap && differentStacks ? EXIT_SUCCESS : EXIT_FAILURE;
}
