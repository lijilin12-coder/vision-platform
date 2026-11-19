// 分页管理 KV Cache：一步一步看块表的变化，再和“按最大长度预留”比一比能同时容纳多少条序列。
//
// 第二、三部分是【模拟】：序列长度是随机生成的，token 用一个 int 代替；结论看比例，不看绝对数字。
//
//   macOS / Linux：clang++ -std=c++17 -I../common paged_demo.cpp -o paged_demo && ./paged_demo
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common paged_demo.cpp && paged_demo.exe
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "block_manager.h"
#include "inftut/common.h"

static bool ok = true;
static void expect(bool cond, const char* what) {
    if (!cond) {
        std::cout << "自检失败：" << what << "\n";
        ok = false;
    }
}

// ---------- 第一部分：看块表 ----------

void printState(const BlockManager& mgr, const std::vector<std::pair<std::string, BlockManager::SeqId>>& seqs) {
    for (const auto& [name, id] : seqs) {
        std::cout << "    " << name << "：长度 " << mgr.length(id) << "，块表 [";
        const auto& table = mgr.blockTable(id);
        for (std::size_t i = 0; i < table.size(); ++i) std::cout << (i ? " " : "") << table[i];
        std::cout << "]\n";
    }
    std::cout << "    物理块的引用计数：";
    for (int b = 0; b < mgr.totalBlocks(); ++b) std::cout << mgr.refCount(b) << " ";
    std::cout << "（空闲 " << mgr.freeBlocks() << " 块）\n";
}

void walkthrough() {
    std::cout << "第一部分：8 个物理块，每块放 4 个 token\n";
    BlockManager mgr(8, 4);

    const auto a = mgr.createSequence();
    for (int i = 0; i < 6; ++i) mgr.append(a, i);
    std::cout << "  1. 序列 A 写入 6 个 token：\n";
    printState(mgr, {{"A", a}});

    const auto b = mgr.createSequence();
    for (int i = 0; i < 5; ++i) mgr.append(b, 50 + i);
    for (int i = 6; i < 10; ++i) mgr.append(a, i);
    std::cout << "  2. 序列 B 写入 5 个 token，然后 A 再写 4 个（A 的块不再相邻）：\n";
    printState(mgr, {{"A", a}, {"B", b}});

    const auto c = mgr.fork(a);
    std::cout << "  3. 从 A 复制出 C（不复制数据，只是引用计数加一）：\n";
    printState(mgr, {{"A", a}, {"B", b}, {"C", c}});

    mgr.append(c, 777);
    std::cout << "  4. C 追加 1 个 token：最后一块没写满而且和 A 共享，先复制再写（写时复制）：\n";
    printState(mgr, {{"A", a}, {"B", b}, {"C", c}});
    expect(mgr.copyOnWriteCount() == 1, "应该发生一次写时复制");
    expect(mgr.read(c, 10) == 777 && mgr.read(c, 9) == 9 && mgr.length(a) == 10, "写时复制之后内容应该正确");

    mgr.release(a);
    std::cout << "  5. A 结束：只有 A 独占的块被释放，和 C 共享的块还在：\n";
    printState(mgr, {{"B", b}, {"C", c}});
    for (int i = 0; i < 10; ++i) expect(mgr.read(c, i) == i, "A 释放之后 C 的内容不应该变");

    mgr.release(b);
    mgr.release(c);
    expect(mgr.freeBlocks() == 8 && mgr.invariantsHold(), "全部释放之后不应该有泄漏");
}

// ---------- 第二部分：能同时容纳多少条序列 ----------

// 一条序列的最终长度（提示词 + 输出）。和第 11 课的负载形状相同：多数短，少数很长。
int randomLength(inftut::Rng& rng) {
    const int prompt = rng.below(10) < 8 ? 32 + static_cast<int>(rng.below(224)) : 512 + static_cast<int>(rng.below(1536));
    const int output = rng.below(10) < 7 ? 16 + static_cast<int>(rng.below(112)) : 128 + static_cast<int>(rng.below(384));
    return prompt + output;
}

void capacity() {
    const int capacityTokens = 65536;  // 整块缓存一共能放这么多个 token
    const int maxLength = 2560;        // 一条序列最长可能有这么长（提示词最长 2047 + 输出最长 511，向上取整）

    std::printf("\n第二部分：缓存一共能放 %d 个 token，一条序列最长 %d。不断加入新序列，直到放不下\n", capacityTokens,
                maxLength);
    // 表头和各行的标签里有汉字，printf 的宽度是按字节算的，所以这里手工对齐
    std::printf("                             序列数   实际使用     利用率   块表总项数\n");

    // 做法一：每条序列按最大长度预留一段连续空间
    {
        inftut::Rng rng(7);
        const int count = capacityTokens / maxLength;
        long stored = 0;
        for (int i = 0; i < count; ++i) stored += randomLength(rng);
        std::printf("  按最大长度预留           %8d %10ld %9.1f%% %12s\n", count, stored,
                    100.0 * static_cast<double>(stored) / capacityTokens, "-");
        expect(count == 25, "按最大长度预留应该只能放 25 条");
    }

    // 做法二：分页，试几种块大小
    int countAt16 = 0;
    for (const int blockSize : {1, 16, 64, 256, 2560}) {
        inftut::Rng rng(7);  // 同样的种子：各种做法面对的是同一串序列
        BlockManager mgr(capacityTokens / blockSize, blockSize);
        int count = 0;
        long stored = 0;
        long tableEntries = 0;
        for (;;) {
            const int length = randomLength(rng);
            const auto id = mgr.createSequence();
            bool fits = true;
            for (int t = 0; t < length && fits; ++t) fits = mgr.append(id, t);
            if (!fits) {  // 放不下了：这一条不算
                mgr.release(id);
                break;
            }
            ++count;
            stored += length;
            tableEntries += static_cast<long>(mgr.blockTable(id).size());
        }
        std::printf("  分页，每块 %4d 个 token %8d %10ld %9.1f%% %12ld\n", blockSize, count, stored,
                    100.0 * static_cast<double>(stored) / capacityTokens, tableEntries);
        if (blockSize == 16) countAt16 = count;
        if (blockSize == 2560) expect(count == 25, "块和最大长度一样大时，分页退化成预留");
        expect(mgr.invariantsHold(), "块管理器内部状态应该自洽");
    }
    expect(countAt16 > 3 * 25, "每块 16 个 token 时，容纳的序列数应该是预留的 3 倍以上");
}

// ---------- 第三部分：共享相同的前缀 ----------

void prefixSharing() {
    const int blockSize = 16;
    const int prefix = 1000;  // 所有请求共用的系统提示词
    const int own = 100;      // 每个请求自己的部分
    const int requests = 16;
    std::printf("\n第三部分：%d 个请求，前 %d 个 token 相同（系统提示词），之后各自再有 %d 个；每块 %d 个 token\n", requests,
                prefix, own, blockSize);

    // 不共享：每个请求从头存一份
    BlockManager separate(4096, blockSize);
    for (int r = 0; r < requests; ++r) {
        const auto id = separate.createSequence();
        for (int t = 0; t < prefix; ++t) separate.append(id, t);
        for (int t = 0; t < own; ++t) separate.append(id, 100000 * (r + 1) + t);
    }

    // 共享：前缀只存一次，每个请求从它复制出来
    BlockManager shared(4096, blockSize);
    const auto base = shared.createSequence();
    for (int t = 0; t < prefix; ++t) shared.append(base, t);
    std::vector<BlockManager::SeqId> ids;
    for (int r = 0; r < requests; ++r) {
        const auto id = shared.fork(base);
        for (int t = 0; t < own; ++t) shared.append(id, 100000 * (r + 1) + t);
        ids.push_back(id);
    }
    shared.release(base);  // 前缀本身不再需要单独保留：它的块被 16 个请求引用着

    std::printf("  不共享：用了 %d 块\n", separate.usedBlocks());
    std::printf("  共享：  用了 %d 块，写时复制 %ld 次\n", shared.usedBlocks(), shared.copyOnWriteCount());

    for (int r = 0; r < requests; ++r) {
        expect(shared.read(ids[static_cast<std::size_t>(r)], prefix - 1) == prefix - 1, "共享的前缀应该读得出来");
        expect(shared.read(ids[static_cast<std::size_t>(r)], prefix) == 100000 * (r + 1), "各自的部分应该互不影响");
    }
    expect(shared.usedBlocks() < separate.usedBlocks() / 4, "共享前缀应该省下大部分块");
    expect(shared.invariantsHold(), "块管理器内部状态应该自洽");
}

int main() {
    walkthrough();
    capacity();
    prefixSharing();
    std::cout << (ok ? "\n自检通过\n" : "\n自检失败\n");
    return ok ? 0 : 1;
}
