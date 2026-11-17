// 分页管理 KV Cache 的块管理器（PagedAttention 里“管内存”的那一半）。
//
// 整块缓存被切成 numBlocks 个大小相同的块，每块能放 blockSize 个 token 的 K 和 V。
// 每条序列有一张“块表”：第 i 项是这条序列的第 i 个逻辑块放在哪个物理块里。
// 块按需分配；多条序列可以共享同一个物理块（引用计数），要写共享的块时先复制一份（写时复制）。
//
// 为了能检查“读出来的就是写进去的”，这里每个 token 只存一个 int，代替真实系统里的一大组 K、V 向量。
#pragma once

#include <cstddef>
#include <map>
#include <vector>

class BlockManager {
public:
    using SeqId = int;

    BlockManager(int numBlocks, int blockSize)
        : blockSize_(blockSize),
          data_(static_cast<std::size_t>(numBlocks) * static_cast<std::size_t>(blockSize), 0),
          refCount_(static_cast<std::size_t>(numBlocks), 0) {
        // 空闲块放在一个栈里。倒着放进去，这样最先分出去的是 0 号块，输出好读。
        for (int b = numBlocks - 1; b >= 0; --b) freeList_.push_back(b);
    }

    // 新建一条空序列。不分配任何块。
    SeqId createSequence() {
        const SeqId id = nextId_++;
        seqs_[id] = Sequence{};
        return id;
    }

    // 在序列末尾追加一个 token。内存不够时返回 false，序列保持原样。
    bool append(SeqId id, int value) {
        Sequence& seq = seqs_.at(id);
        const int offset = seq.length % blockSize_;  // 新 token 落在最后一个块里的第几格
        if (offset == 0) {
            // 最后一个块正好写满（或者序列还是空的）：要一个新块
            const int block = allocate();
            if (block < 0) return false;
            seq.table.push_back(block);
        } else if (refCount_[static_cast<std::size_t>(seq.table.back())] > 1) {
            // 最后一个块没写满，但是别的序列也在用它：不能直接写，先复制一份自己用（写时复制）
            const int shared = seq.table.back();
            const int own = allocate();
            if (own < 0) return false;
            for (int i = 0; i < offset; ++i) cell(own, i) = cell(shared, i);
            --refCount_[static_cast<std::size_t>(shared)];
            seq.table.back() = own;
            ++copies_;
        }
        cell(seq.table.back(), offset) = value;
        ++seq.length;
        return true;
    }

    // 读序列的第 pos 个 token：先查块表找到物理块，再取块内的偏移。
    // 真实系统里，注意力的计算就是这样按块表去取每一个历史 token 的 K 和 V。
    int read(SeqId id, int pos) const {
        const Sequence& seq = seqs_.at(id);
        const int block = seq.table[static_cast<std::size_t>(pos / blockSize_)];
        return data_[static_cast<std::size_t>(block) * static_cast<std::size_t>(blockSize_) +
                     static_cast<std::size_t>(pos % blockSize_)];
    }

    // 复制一条序列：新序列和原序列内容相同，共享全部物理块，不复制任何数据。
    SeqId fork(SeqId parent) {
        const Sequence copy = seqs_.at(parent);  // 只复制了块表和长度
        for (const int block : copy.table) ++refCount_[static_cast<std::size_t>(block)];
        const SeqId id = nextId_++;
        seqs_[id] = copy;
        return id;
    }

    // 序列结束：它用到的每个块引用计数减一，减到 0 的块回到空闲栈。
    void release(SeqId id) {
        for (const int block : seqs_.at(id).table) {
            if (--refCount_[static_cast<std::size_t>(block)] == 0) freeList_.push_back(block);
        }
        seqs_.erase(id);
    }

    // ---- 查询 ----
    int blockSize() const { return blockSize_; }
    int totalBlocks() const { return static_cast<int>(refCount_.size()); }
    int freeBlocks() const { return static_cast<int>(freeList_.size()); }
    int usedBlocks() const { return totalBlocks() - freeBlocks(); }
    int length(SeqId id) const { return seqs_.at(id).length; }
    const std::vector<int>& blockTable(SeqId id) const { return seqs_.at(id).table; }
    int refCount(int block) const { return refCount_[static_cast<std::size_t>(block)]; }
    long copyOnWriteCount() const { return copies_; }

    // 检查内部状态是否自洽。测试里每做一步操作就检查一次。
    bool invariantsHold() const {
        // 1. 每个块的引用计数 = 有多少张块表里出现了它
        std::vector<int> counted(refCount_.size(), 0);
        for (const auto& entry : seqs_) {
            const Sequence& seq = entry.second;
            // 2. 块表的项数正好够放下这条序列
            if (static_cast<int>(seq.table.size()) != (seq.length + blockSize_ - 1) / blockSize_) return false;
            for (const int block : seq.table) ++counted[static_cast<std::size_t>(block)];
        }
        if (counted != refCount_) return false;
        // 3. 空闲栈里的块引用计数都是 0，而且每个引用计数为 0 的块都在空闲栈里（没有泄漏）
        int zero = 0;
        for (const int count : refCount_) zero += count == 0 ? 1 : 0;
        if (zero != freeBlocks()) return false;
        for (const int block : freeList_) {
            if (refCount_[static_cast<std::size_t>(block)] != 0) return false;
        }
        return true;
    }

private:
    struct Sequence {
        std::vector<int> table;  // 块表：逻辑块号 → 物理块号
        int length = 0;          // 已经存了多少个 token
    };

    int allocate() {
        if (freeList_.empty()) return -1;
        const int block = freeList_.back();
        freeList_.pop_back();
        refCount_[static_cast<std::size_t>(block)] = 1;
        return block;
    }

    int& cell(int block, int offset) {
        return data_[static_cast<std::size_t>(block) * static_cast<std::size_t>(blockSize_) +
                     static_cast<std::size_t>(offset)];
    }

    int blockSize_;
    std::vector<int> data_;       // 全部物理块，一次申请好：第 b 块是 data_[b * blockSize_ ...]
    std::vector<int> refCount_;   // 每个物理块被多少条序列引用
    std::vector<int> freeList_;   // 空闲的物理块（当作栈用）
    std::map<SeqId, Sequence> seqs_;
    SeqId nextId_ = 0;
    long copies_ = 0;             // 发生了多少次写时复制
};
