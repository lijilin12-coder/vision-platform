// 内存规划：在加载模型时就决定“每个中间张量放在哪一块缓冲区里”，推理时不再申请内存。
//
//   computeLastUse   每个张量最后一次被用到是在第几步（生命周期的终点）
//   planMemory       按生命周期给每个张量分配缓冲区：生命周期不重叠的张量共用同一块
//   PlannedExecutor  按规划执行：缓冲区在构造时一次申请好，run() 里不申请内存
#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "../lesson02/graph.h"

namespace inftut {

constexpr std::size_t kNoBuffer = std::numeric_limits<std::size_t>::max();

// Input 的数据在调用者手里，Constant 的数据在模型的权重里：它们不需要缓冲区。
inline bool needsBuffer(const Step& step) { return step.node.op != OpType::Input && step.node.op != OpType::Constant; }

// ---------- 生命周期 ----------
// 第 i 步的输出从第 i 步开始存在，到“最后一个用它的步骤”结束。
// 返回 lastUse[i]：最后一次用到第 i 步输出的步骤下标。
//   没有人用：lastUse[i] = i（算完就可以丢）
//   是整个模型的输出：lastUse[i] = 步骤总数（一直留到推理结束）
inline std::vector<std::size_t> computeLastUse(const Plan& plan) {
    std::vector<std::size_t> lastUse(plan.steps.size());
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        lastUse[i] = i;
        for (std::size_t src : plan.steps[i].inputs) lastUse[src] = i;  // 步骤是按顺序走的，后面的会覆盖前面的
    }
    lastUse[plan.outputStep] = plan.steps.size();
    return lastUse;
}

// ---------- 规划 ----------

struct MemoryPlan {
    std::vector<std::size_t> bufferOf;    // bufferOf[i]：第 i 步的输出放在第几块缓冲区；不需要缓冲区时是 kNoBuffer
    std::vector<std::size_t> bufferSize;  // 每块缓冲区要多大（元素个数）
    std::vector<bool> inPlace;            // inPlace[i]：第 i 步是不是原地执行（输出直接写在输入的缓冲区里）

    std::size_t totalElements() const {
        std::size_t total = 0;
        for (std::size_t s : bufferSize) total += s;
        return total;
    }
};

// 这种算子的输出能不能直接写在它的输入上。
// 条件是“每个输出元素只依赖同一位置的输入元素”。卷积、池化、全连接都不满足。
inline bool canRunInPlace(OpType op) {
    return op == OpType::Relu || op == OpType::Add || op == OpType::BatchNorm || op == OpType::Softmax;
}

// 贪心地分配缓冲区。按执行顺序走一遍，模拟“申请”和“归还”：
//   1. 轮到第 i 步：给它的输出找一块空闲的缓冲区（没有就新开一块）
//   2. 第 i 步做完：它的输入里，生命周期到此为止的，把缓冲区归还，后面的步骤可以再用
// allowInPlace：允许原地执行。输入的生命周期正好在这一步结束时，输出直接接手输入的缓冲区。
inline MemoryPlan planMemory(const Plan& plan, const std::vector<std::size_t>& lastUse, bool allowInPlace) {
    const std::size_t count = plan.steps.size();
    MemoryPlan mem;
    mem.bufferOf.assign(count, kNoBuffer);
    mem.inPlace.assign(count, false);
    std::vector<bool> isFree;  // 每块缓冲区现在是不是空闲

    for (std::size_t i = 0; i < count; ++i) {
        const Step& step = plan.steps[i];
        if (!needsBuffer(step)) continue;
        const std::size_t need = step.shape.elements();

        // 原地：找一个“有缓冲区、而且这是最后一次被用到”的输入，直接接手它的缓冲区
        std::size_t taken = kNoBuffer;  // 被接手的那个输入是第几步的输出
        if (allowInPlace && canRunInPlace(step.node.op)) {
            for (std::size_t src : step.inputs)
                if (mem.bufferOf[src] != kNoBuffer && lastUse[src] == i && plan.steps[src].shape.elements() == need) {
                    taken = src;
                    break;
                }
        }

        if (taken != kNoBuffer) {
            mem.bufferOf[i] = mem.bufferOf[taken];
            mem.inPlace[i] = true;
        } else {
            // 在空闲的缓冲区里挑：够大的里面选最小的；都不够大就选最大的那块，把它扩大
            std::size_t best = kNoBuffer;
            for (std::size_t b = 0; b < mem.bufferSize.size(); ++b) {
                if (!isFree[b]) continue;
                if (best == kNoBuffer) {
                    best = b;
                    continue;
                }
                const bool bestFits = mem.bufferSize[best] >= need;
                const bool thisFits = mem.bufferSize[b] >= need;
                if (thisFits && (!bestFits || mem.bufferSize[b] < mem.bufferSize[best])) best = b;
                if (!thisFits && !bestFits && mem.bufferSize[b] > mem.bufferSize[best]) best = b;
            }
            if (best == kNoBuffer) {  // 没有空闲的：新开一块
                best = mem.bufferSize.size();
                mem.bufferSize.push_back(0);
                isFree.push_back(false);
            }
            if (mem.bufferSize[best] < need) mem.bufferSize[best] = need;
            isFree[best] = false;
            mem.bufferOf[i] = best;
        }

        // 归还：这一步的输入里，生命周期到此为止的（被原地接手的那个不还，它的缓冲区现在归输出用）
        for (std::size_t src : step.inputs)
            if (mem.bufferOf[src] != kNoBuffer && lastUse[src] == i && src != taken) isFree[mem.bufferOf[src]] = true;
        // 输出没有人用：算完立刻归还
        if (lastUse[i] == i) isFree[mem.bufferOf[i]] = true;
    }
    return mem;
}

// ---------- 用来对比的三个数 ----------

// 不做任何规划（第 2 课的执行器）：每个中间张量各占一块，全部留到推理结束。
inline std::size_t totalIntermediateElements(const Plan& plan) {
    std::size_t total = 0;
    for (const Step& s : plan.steps)
        if (needsBuffer(s)) total += s.shape.elements();
    return total;
}

// 任何规划都不可能低于的下界（不算原地执行）：执行到某一步时，“必须同时存在”的张量加起来最多有多大。
inline std::size_t peakLiveElements(const Plan& plan, const std::vector<std::size_t>& lastUse) {
    std::size_t peak = 0;
    for (std::size_t now = 0; now < plan.steps.size(); ++now) {
        std::size_t live = 0;
        for (std::size_t i = 0; i <= now; ++i)
            if (needsBuffer(plan.steps[i]) && lastUse[i] >= now) live += plan.steps[i].shape.elements();
        if (live > peak) peak = live;
    }
    return peak;
}

// ---------- 按规划执行 ----------

class PlannedExecutor {
public:
    PlannedExecutor(Plan plan, MemoryPlan mem) : plan_(std::move(plan)), mem_(std::move(mem)) {
        // 所有缓冲区在这里一次申请好。之后 Tensor::resize 只改形状，容量够用，不会再申请内存。
        buffers_.resize(mem_.bufferSize.size());
        for (std::size_t b = 0; b < buffers_.size(); ++b) buffers_[b].data.reserve(mem_.bufferSize[b]);
    }

    // 执行一次推理。返回的引用指向内部的缓冲区，下一次 run() 之后内容会被覆盖。
    const Tensor& run(const Tensor& input) {
        std::vector<const Tensor*>& in = scratch_;  // 连这个小数组也复用，推理过程中一次内存都不申请
        for (std::size_t i = 0; i < plan_.steps.size(); ++i) {
            const Step& step = plan_.steps[i];
            if (!needsBuffer(step)) continue;
            in.clear();
            for (std::size_t src : step.inputs) in.push_back(tensorOf(src, input));
            runNode(step.node, in, buffers_[mem_.bufferOf[i]]);
        }
        return *tensorOf(plan_.outputStep, input);
    }

    // 每块缓冲区当前的内存地址。示例用它来检查“推理前后地址没变”，也就是没有重新申请过内存。
    std::vector<const float*> bufferAddresses() const {
        std::vector<const float*> addresses;
        for (const Tensor& t : buffers_) addresses.push_back(t.data.data());
        return addresses;
    }

private:
    const Tensor* tensorOf(std::size_t stepIndex, const Tensor& input) const {
        const Step& step = plan_.steps[stepIndex];
        if (step.node.op == OpType::Input) return &input;                  // 直接用调用者的内存，不复制
        if (step.node.op == OpType::Constant) return &step.node.weights[0];  // 直接用权重，不复制
        return &buffers_[mem_.bufferOf[stepIndex]];
    }

    Plan plan_;
    MemoryPlan mem_;
    std::vector<Tensor> buffers_;
    std::vector<const Tensor*> scratch_;
};

}  // namespace inftut
