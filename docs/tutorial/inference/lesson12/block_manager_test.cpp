// BlockManager 的测试：先是几个手写的场景，然后是一个随机测试。
//
// 随机测试的做法：同时维护一份“最朴素的正确答案”（每条序列一个 std::vector<int>），
// 随机地新建、追加、复制、释放，每一步之后检查块管理器的内部状态，并且把每条序列读出来和正确答案比较。
// 这种“和一个简单实现对拍”的测试方法，竞赛里常用，对内存管理这类代码特别有效。
#include <iostream>
#include <map>
#include <vector>

#include "block_manager.h"
#include "inftut/common.h"

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cout << "失败：" #cond "（第 " << __LINE__ << " 行）\n";          \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

// 场景一：按需分配。块只在写满之后才多要一个。
void testAllocateOnDemand() {
    BlockManager mgr(8, 4);
    const auto a = mgr.createSequence();
    CHECK(mgr.usedBlocks() == 0);  // 新建序列不占内存
    for (int i = 0; i < 4; ++i) CHECK(mgr.append(a, i));
    CHECK(mgr.usedBlocks() == 1);  // 4 个 token 正好一块
    CHECK(mgr.append(a, 4));
    CHECK(mgr.usedBlocks() == 2);  // 第 5 个 token 才要第二块
    for (int i = 0; i < 5; ++i) CHECK(mgr.read(a, i) == i);
    mgr.release(a);
    CHECK(mgr.freeBlocks() == 8);
    CHECK(mgr.invariantsHold());
}

// 场景二：内存用完时 append 返回 false，序列不变；别的序列释放之后可以继续。
void testOutOfMemory() {
    BlockManager mgr(2, 4);
    const auto a = mgr.createSequence();
    const auto b = mgr.createSequence();
    for (int i = 0; i < 4; ++i) CHECK(mgr.append(a, i));
    for (int i = 0; i < 4; ++i) CHECK(mgr.append(b, 10 + i));
    CHECK(!mgr.append(a, 99));  // 两块都用完了
    CHECK(mgr.length(a) == 4);
    CHECK(mgr.invariantsHold());
    mgr.release(b);
    CHECK(mgr.append(a, 99));   // b 的块还回来了
    CHECK(mgr.read(a, 4) == 99);
    mgr.release(a);
    CHECK(mgr.freeBlocks() == 2);
}

// 场景三：复制和写时复制。
void testForkAndCopyOnWrite() {
    BlockManager mgr(8, 4);
    const auto a = mgr.createSequence();
    for (int i = 0; i < 6; ++i) CHECK(mgr.append(a, i));  // 一块写满，一块写了一半
    const auto b = mgr.fork(a);
    CHECK(mgr.usedBlocks() == 2);                 // 复制没有多占内存
    CHECK(mgr.blockTable(a) == mgr.blockTable(b));
    CHECK(mgr.refCount(mgr.blockTable(a)[0]) == 2);

    CHECK(mgr.append(b, 100));                    // b 要写共享的半满块：触发写时复制
    CHECK(mgr.copyOnWriteCount() == 1);
    CHECK(mgr.usedBlocks() == 3);
    CHECK(mgr.blockTable(a)[0] == mgr.blockTable(b)[0]);  // 写满的那块仍然共享
    CHECK(mgr.blockTable(a)[1] != mgr.blockTable(b)[1]);  // 半满的那块各用各的

    CHECK(mgr.append(a, 200));                    // a 现在独占自己的半满块：不用再复制
    CHECK(mgr.copyOnWriteCount() == 1);
    CHECK(mgr.read(a, 6) == 200);
    CHECK(mgr.read(b, 6) == 100);                 // 互不影响
    for (int i = 0; i < 6; ++i) CHECK(mgr.read(a, i) == i && mgr.read(b, i) == i);

    mgr.release(a);
    CHECK(mgr.usedBlocks() == 2);                 // 共享的那块还被 b 用着，没有释放
    for (int i = 0; i < 6; ++i) CHECK(mgr.read(b, i) == i);
    mgr.release(b);
    CHECK(mgr.freeBlocks() == 8);
    CHECK(mgr.invariantsHold());
}

// 随机测试：和 std::vector<int> 对拍。
void testRandomAgainstReference() {
    inftut::Rng rng(12);
    BlockManager mgr(64, 4);
    std::map<BlockManager::SeqId, std::vector<int>> reference;
    std::vector<BlockManager::SeqId> alive;
    int appendsRefused = 0;

    for (int step = 0; step < 4000; ++step) {
        const auto op = rng.below(100);
        if (alive.empty() || op < 5) {  // 新建
            const auto id = mgr.createSequence();
            reference[id] = {};
            alive.push_back(id);
        } else if (op < 75) {           // 追加
            const auto id = alive[rng.below(static_cast<std::uint32_t>(alive.size()))];
            const int value = static_cast<int>(rng.below(1000000));
            if (mgr.append(id, value)) {
                reference[id].push_back(value);
            } else {
                ++appendsRefused;
            }
        } else if (op < 85) {           // 复制
            const auto parent = alive[rng.below(static_cast<std::uint32_t>(alive.size()))];
            const auto id = mgr.fork(parent);
            reference[id] = reference[parent];
            alive.push_back(id);
        } else {                        // 释放
            const auto index = rng.below(static_cast<std::uint32_t>(alive.size()));
            mgr.release(alive[index]);
            reference.erase(alive[index]);
            alive.erase(alive.begin() + index);
        }

        CHECK(mgr.invariantsHold());
        if (step % 50 == 0 || failures > 0) {  // 每隔一段把所有序列完整地读一遍
            for (const auto id : alive) {
                const auto& expected = reference[id];
                CHECK(mgr.length(id) == static_cast<int>(expected.size()));
                for (int pos = 0; pos < mgr.length(id); ++pos) {
                    CHECK(mgr.read(id, pos) == expected[static_cast<std::size_t>(pos)]);
                }
            }
        }
        if (failures > 0) return;
    }

    for (const auto id : alive) mgr.release(id);
    CHECK(mgr.freeBlocks() == mgr.totalBlocks());  // 全部释放之后，一块都不能少
    CHECK(mgr.invariantsHold());
    std::cout << "随机测试：4000 步，写时复制 " << mgr.copyOnWriteCount() << " 次，内存不足被拒绝 " << appendsRefused
              << " 次，结束后空闲块 " << mgr.freeBlocks() << "/" << mgr.totalBlocks() << "\n";
    CHECK(mgr.copyOnWriteCount() > 0);  // 确认随机测试真的走到了这两条路径
    CHECK(appendsRefused > 0);
}

int main() {
    testAllocateOnDemand();
    testOutOfMemory();
    testForkAndCopyOnWrite();
    testRandomAgainstReference();
    std::cout << (failures == 0 ? "全部测试通过\n" : "有测试失败\n");
    return failures == 0 ? 0 : 1;
}
