// 固定大小的小块池：一次申请一大块内存，切成等大的小格，用“空闲链表”管理。
//
//   macOS / Linux：clang++ -std=c++17 -O2 block_pool.cpp -o block_pool && ./block_pool
//   Windows：      cl /std:c++17 /O2 /EHsc /utf-8 block_pool.cpp && block_pool.exe
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <vector>

// 一个只能分配固定大小小块的池。不是线程安全的：每个线程用自己的池，或者在外面加锁。
class FixedBlockPool {
public:
    FixedBlockPool(std::size_t blockSize, std::size_t blockCount)
        : blockSize_(std::max(roundUp(blockSize), sizeof(FreeNode))),
          blockCount_(blockCount),
          memory_(new std::byte[blockSize_ * blockCount]) {
        // 把整块内存切成 blockCount 个小格，全部串到空闲链表上。
        // 空闲的小格里反正没有数据，就借用它自己的前几个字节来存“下一个空闲小格在哪里”。
        for (std::size_t i = 0; i < blockCount; ++i) {
            auto* node = reinterpret_cast<FreeNode*>(memory_.get() + i * blockSize_);
            node->next = freeList_;
            freeList_ = node;
        }
    }

    // 取一个小格：从链表头上摘下一个。池用完时返回空指针。
    void* allocate() {
        if (freeList_ == nullptr) return nullptr;
        FreeNode* node = freeList_;
        freeList_ = node->next;
        ++inUse_;
        return node;
    }

    // 还一个小格：把它挂回链表头。
    void deallocate(void* block) {
        auto* node = static_cast<FreeNode*>(block);
        node->next = freeList_;
        freeList_ = node;
        --inUse_;
    }

    std::size_t inUse() const { return inUse_; }
    std::size_t capacity() const { return blockCount_; }
    bool owns(const void* p) const {
        const auto* b = static_cast<const std::byte*>(p);
        return b >= memory_.get() && b < memory_.get() + blockSize_ * blockCount_;
    }

private:
    struct FreeNode {
        FreeNode* next;
    };
    // 小格的大小向上取整到指针大小的整数倍，保证每个小格的起始地址都适合存放一般的对象。
    static std::size_t roundUp(std::size_t n) {
        const std::size_t a = alignof(std::max_align_t);
        return (n + a - 1) / a * a;
    }

    std::size_t blockSize_;
    std::size_t blockCount_;
    std::unique_ptr<std::byte[]> memory_;
    FreeNode* freeList_ = nullptr;
    std::size_t inUse_ = 0;
};

// 一个典型的小对象：连通块的信息，40 字节
struct Blob {
    double area;
    double centerX;
    double centerY;
    int left, top, width, height;
};

using Clock = std::chrono::steady_clock;
volatile double sink = 0;

template <typename Fn>
double medianNsPerItem(int repeats, int items, Fn&& fn) {
    std::vector<double> times;
    for (int i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        fn();
        times.push_back(std::chrono::duration<double, std::nano>(Clock::now() - start).count() / items);
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

int main() {
    const int count = 100000;
    std::vector<Blob*> blobs(static_cast<std::size_t>(count));

    // 做法一：每个对象单独 new / delete
    const double newDelete = medianNsPerItem(51, count, [&] {
        for (int i = 0; i < count; ++i) blobs[static_cast<std::size_t>(i)] = new Blob{1.0 * i, 0, 0, 0, 0, 0, 0};
        sink = blobs[static_cast<std::size_t>(count / 2)]->area;
        for (auto* b : blobs) delete b;
    });

    // 做法二：从池里取，用完还回池里
    FixedBlockPool pool(sizeof(Blob), static_cast<std::size_t>(count));
    const double pooled = medianNsPerItem(51, count, [&] {
        for (int i = 0; i < count; ++i) {
            void* memory = pool.allocate();
            blobs[static_cast<std::size_t>(i)] = new (memory) Blob{1.0 * i, 0, 0, 0, 0, 0, 0};  // 在这块内存上构造对象
        }
        sink = blobs[static_cast<std::size_t>(count / 2)]->area;
        for (auto* b : blobs) {
            b->~Blob();           // 手动调用析构函数
            pool.deallocate(b);   // 把内存还给池
        }
    });

    // 做法三：根本不单独申请，直接放进 vector
    const double inVector = medianNsPerItem(51, count, [&] {
        std::vector<Blob> all;
        all.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) all.push_back(Blob{1.0 * i, 0, 0, 0, 0, 0, 0});
        sink = all[static_cast<std::size_t>(count / 2)].area;
    });

    std::cout << "创建并销毁 " << count << " 个 40 字节的对象，平均每个：\n";
    std::cout << "  每个单独 new / delete：  " << newDelete << " 纳秒\n";
    std::cout << "  从固定大小的池里取：     " << pooled << " 纳秒\n";
    std::cout << "  直接放进 std::vector：   " << inVector << " 纳秒\n";

    // 正确性：池用完时返回空；还回去之后又可以取；取到的内存在池的范围内
    FixedBlockPool small(sizeof(Blob), 2);
    void* a = small.allocate();
    void* b = small.allocate();
    const bool exhausted = small.allocate() == nullptr;
    small.deallocate(a);
    void* c = small.allocate();
    const bool ok = a != nullptr && b != nullptr && a != b && exhausted && c == a && small.owns(c) && small.inUse() == 2 &&
                    pool.inUse() == 0;
    std::cout << (ok ? "池的行为符合预期\n" : "池的行为不符合预期\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
