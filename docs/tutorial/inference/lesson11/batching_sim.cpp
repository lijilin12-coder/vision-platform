// 推理服务的三种调度方式：逐条处理、静态攒批、连续攒批（continuous batching）。
//
// 这是一个【模拟器】：不做真正的推理，只按一个假设的成本模型推进时间，
// 用来比较三种调度方式的吞吐和延时。成本模型的参数在 CostModel 里，可以随意修改。
//
// 时间全部用整数（微秒）表示，随机数用教程自带的 Rng，所以在任何平台上输出都完全相同。
//
//   macOS / Linux：clang++ -std=c++17 -I../common batching_sim.cpp -o batching_sim && ./batching_sim
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common batching_sim.cpp && batching_sim.exe
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "inftut/common.h"

using Us = std::int64_t;  // 时间，单位微秒

// ---------- 请求 ----------

struct Request {
    int id = 0;
    Us arriveUs = 0;      // 到达服务的时刻
    int promptLen = 0;    // 提示词有多少个 token
    int outputLen = 0;    // 要生成多少个 token（真实系统里事先不知道，模拟器里提前定好）

    int generated = 0;    // 已经生成了多少个 token
    Us firstTokenUs = -1; // 第一个 token 生成出来的时刻
    Us finishUs = -1;     // 最后一个 token 生成出来的时刻
};

// ---------- 成本模型（假设）----------
//
// 一次前向（下面叫“一步”）的耗时 = 固定开销
//                                + 每条正在解码的序列一份开销
//                                + 这一步里要做 Prefill 的提示词 token 数 × 每个 token 的开销
//
// 固定开销远大于每条序列的开销：这就是攒批能提高吞吐的原因。
// Prefill 时提示词里的 token 是并行处理的，所以每个 token 的开销比解码一步小得多。
struct CostModel {
    Us stepFixedUs = 20000;      // 每一步的固定开销：20 ms
    Us stepPerSeqUs = 500;       // 每条正在解码的序列：0.5 ms
    Us prefillPerTokenUs = 20;   // Prefill 里每个提示词 token：0.02 ms
};

// ---------- 记录每一步里每个位置在干什么（只用来画示意图）----------

struct Trace {
    bool enabled = false;
    std::vector<std::string> rows;  // rows[位置][步]：字母 = 在给这个请求算 token；'.' = 白算；' ' = 空着

    void begin(int slots) {
        if (enabled) rows.assign(static_cast<std::size_t>(slots), std::string());
    }
    void column(const std::vector<char>& cells) {
        if (!enabled) return;
        for (std::size_t i = 0; i < rows.size(); ++i) rows[i].push_back(cells[i]);
    }
    void idle(Us steps) {
        if (!enabled) return;
        for (Us s = 0; s < steps; ++s)
            for (auto& row : rows) row.push_back(' ');
    }
};

inline char letterOf(int id) { return static_cast<char>('A' + id % 26); }

struct RunStats {
    Us endUs = 0;              // 最后一个请求完成的时刻
    std::int64_t steps = 0;    // 一共跑了多少步
    std::int64_t slotSteps = 0;   // 所有步的批大小之和（平均批大小 = slotSteps / steps）
    std::int64_t wastedSlots = 0; // 其中白算的：序列已经结束，却还占着批里的位置
    std::int64_t paddedPrefillTokens = 0;  // Prefill 时为了对齐而填充的 token 数
};

// ---------- 调度方式一、三：连续攒批（maxBatch = 1 时就是逐条处理）----------
//
// 每一步开始之前：把已经到达的请求加入批里，直到批满。
// 每一步结束之后：批里每条序列多了一个 token；生成完的立刻离开，位置空出来。
RunStats runContinuous(std::vector<Request>& reqs, const CostModel& cost, int maxBatch, Trace* trace = nullptr) {
    RunStats stats;
    std::vector<int> slots(static_cast<std::size_t>(maxBatch), -1);  // 每个位置上是哪个请求；-1 表示空
    int running = 0;
    std::size_t next = 0;  // 下一个还没被接纳的请求（请求按到达时间排好序）
    Us now = 0;
    if (trace) trace->begin(maxBatch);

    while (next < reqs.size() || running > 0) {
        // 1. 接纳：有空位，并且有已经到达的请求
        Us prefillTokens = 0;
        int decoding = running;  // 这一步之前就在批里的序列：它们这一步做解码
        for (auto& slot : slots) {
            if (next >= reqs.size() || reqs[next].arriveUs > now) break;
            if (slot != -1) continue;
            slot = static_cast<int>(next);
            prefillTokens += reqs[next].promptLen;
            ++running;
            ++next;
        }
        if (running == 0) {  // 没有事情可做：直接跳到下一个请求到达的时刻
            if (trace) trace->idle(reqs[next].arriveUs - now);
            now = reqs[next].arriveUs;
            continue;
        }

        // 2. 跑一步
        now += cost.stepFixedUs + cost.stepPerSeqUs * decoding + cost.prefillPerTokenUs * prefillTokens;
        ++stats.steps;
        stats.slotSteps += running;

        // 3. 每条序列得到一个 token；生成完的离开
        std::vector<char> cells(slots.size(), ' ');
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i] == -1) continue;
            Request& r = reqs[static_cast<std::size_t>(slots[i])];
            cells[i] = letterOf(r.id);
            if (r.generated == 0) r.firstTokenUs = now;
            if (++r.generated == r.outputLen) {
                r.finishUs = now;
                slots[i] = -1;
                --running;
            }
        }
        if (trace) trace->column(cells);
    }
    stats.endUs = now;
    return stats;
}

// ---------- 调度方式二：静态攒批 ----------
//
// 等凑够 batchSize 个请求，或者第一个请求已经等了 timeoutUs，就把现有的请求组成一批。
// 一批一旦开始，中途不加人也不减人，直到批里【最长】的那条生成完。
RunStats runStatic(std::vector<Request>& reqs, const CostModel& cost, int batchSize, Us timeoutUs,
                   Trace* trace = nullptr) {
    RunStats stats;
    std::size_t next = 0;
    Us now = 0;
    if (trace) trace->begin(batchSize);

    while (next < reqs.size()) {
        // 1. 组批：从第一个请求可以被处理的时刻开始等
        const Us waitFrom = std::max(now, reqs[next].arriveUs);
        Us closeAt = waitFrom + timeoutUs;  // 最晚等到这个时刻
        const std::size_t last = next + static_cast<std::size_t>(batchSize) - 1;
        if (last < reqs.size()) closeAt = std::min(closeAt, std::max(waitFrom, reqs[last].arriveUs));  // 凑够了就不等
        std::vector<std::size_t> batch;
        while (next < reqs.size() && batch.size() < static_cast<std::size_t>(batchSize) &&
               reqs[next].arriveUs <= closeAt) {
            batch.push_back(next++);
        }
        if (trace) trace->idle(closeAt - now);
        now = closeAt;
        const auto n = static_cast<Us>(batch.size());

        // 2. Prefill：所有提示词填充到最长的那条的长度，一起做
        int maxPrompt = 0;
        int maxOutput = 0;
        Us realPromptTokens = 0;
        for (const std::size_t i : batch) {
            maxPrompt = std::max(maxPrompt, reqs[i].promptLen);
            maxOutput = std::max(maxOutput, reqs[i].outputLen);
            realPromptTokens += reqs[i].promptLen;
        }
        stats.paddedPrefillTokens += n * maxPrompt - realPromptTokens;

        // 3. 一共跑 maxOutput 步：第 1 步是 Prefill，后面每一步是解码
        for (int step = 0; step < maxOutput; ++step) {
            if (step == 0) {
                now += cost.stepFixedUs + cost.prefillPerTokenUs * n * maxPrompt;
            } else {
                now += cost.stepFixedUs + cost.stepPerSeqUs * n;  // 已经结束的序列也照算不误
            }
            ++stats.steps;
            stats.slotSteps += n;
            std::vector<char> cells(static_cast<std::size_t>(batchSize), ' ');
            for (std::size_t k = 0; k < batch.size(); ++k) {
                Request& r = reqs[batch[k]];
                if (r.generated == r.outputLen) {  // 早就生成完了，但是批还没结束
                    ++stats.wastedSlots;
                    cells[k] = '.';
                    continue;
                }
                cells[k] = letterOf(r.id);
                if (r.generated == 0) r.firstTokenUs = now;
                if (++r.generated == r.outputLen) r.finishUs = now;
            }
            if (trace) trace->column(cells);
        }
    }
    stats.endUs = now;
    return stats;
}

// ---------- 负载 ----------

// 生成 count 个请求。到达间隔在 [0, 2 × meanGapUs) 上均匀分布，平均是 meanGapUs。
// 提示词和输出的长度都是“多数短、少数很长”，这是对话类负载的常见形状。
std::vector<Request> makeWorkload(int count, Us meanGapUs, std::uint64_t seed) {
    inftut::Rng rng(seed);
    std::vector<Request> reqs;
    Us t = 0;
    for (int i = 0; i < count; ++i) {
        Request r;
        r.id = i;
        t += static_cast<Us>(rng.below(static_cast<std::uint32_t>(2 * meanGapUs)));
        r.arriveUs = t;
        r.promptLen = rng.below(10) < 8 ? 32 + static_cast<int>(rng.below(224))      // 80%：32～255
                                        : 512 + static_cast<int>(rng.below(1536));   // 20%：512～2047
        r.outputLen = rng.below(10) < 7 ? 16 + static_cast<int>(rng.below(112))      // 70%：16～127
                                        : 128 + static_cast<int>(rng.below(384));    // 30%：128～511
        reqs.push_back(r);
    }
    return reqs;
}

// ---------- 指标 ----------

// 第 p 百分位（p 在 0～100 之间）：排序后，至少有 p% 的值不大于它的那个值。
double percentile(std::vector<double> values, int p) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    std::size_t rank = (n * static_cast<std::size_t>(p) + 99) / 100;  // 向上取整
    if (rank < 1) rank = 1;
    return values[rank - 1];
}

struct Metrics {
    double tokensPerSec = 0;
    double ttftP50 = 0, ttftP99 = 0;        // 首字延时（秒）
    double latencyP50 = 0, latencyP99 = 0;  // 从到达到最后一个 token（秒）
    double avgBatch = 0;
    double wastedPercent = 0;
};

// 检查模拟结果是否自洽：每个请求都恰好生成了它要的 token 数，时间顺序合理。
bool consistent(const std::vector<Request>& reqs) {
    for (const auto& r : reqs) {
        if (r.generated != r.outputLen) return false;
        if (r.firstTokenUs <= r.arriveUs) return false;
        if (r.finishUs < r.firstTokenUs) return false;
    }
    return true;
}

Metrics summarize(const std::vector<Request>& reqs, const RunStats& stats) {
    Metrics m;
    std::int64_t tokens = 0;
    std::vector<double> ttft, latency;
    for (const auto& r : reqs) {
        tokens += r.outputLen;
        ttft.push_back(static_cast<double>(r.firstTokenUs - r.arriveUs) / 1e6);
        latency.push_back(static_cast<double>(r.finishUs - r.arriveUs) / 1e6);
    }
    m.tokensPerSec = static_cast<double>(tokens) / (static_cast<double>(stats.endUs - reqs.front().arriveUs) / 1e6);
    m.ttftP50 = percentile(ttft, 50);
    m.ttftP99 = percentile(ttft, 99);
    m.latencyP50 = percentile(latency, 50);
    m.latencyP99 = percentile(latency, 99);
    m.avgBatch = static_cast<double>(stats.slotSteps) / static_cast<double>(stats.steps);
    m.wastedPercent = 100.0 * static_cast<double>(stats.wastedSlots) / static_cast<double>(stats.slotSteps);
    return m;
}

void printRow(const char* name, const Metrics& m) {
    // name 都是四个汉字，所以不用 %-10s 这类按字节数对齐的写法
    std::printf("  %s %8.0f %9.2f %9.2f %10.2f %10.2f %8.1f %7.1f%%\n", name, m.tokensPerSec, m.ttftP50, m.ttftP99,
                m.latencyP50, m.latencyP99, m.avgBatch, m.wastedPercent);
}

// ---------- 第一部分：一个能看清楚的小例子 ----------

// 6 个请求，批大小 3。为了让图上的一格正好是一步，这里每一步的耗时固定为 1。
bool toyExample() {
    const CostModel unit{1, 0, 0};
    //                         id 到达 提示词 输出
    const std::vector<Request> base = {{0, 0, 1, 2}, {1, 0, 1, 9}, {2, 0, 1, 3},
                                       {3, 1, 1, 4}, {4, 2, 1, 2}, {5, 3, 1, 3}};
    std::cout << "小例子：6 个请求，批大小 3。每行是批里的一个位置，每列是一步。\n"
              << "        字母 = 在给这个请求算 token；'.' = 白算（序列已结束，还占着位置）；空格 = 位置空着\n"
              << "        请求（到达时刻/输出长度）：A(0/2) B(0/9) C(0/3) D(1/4) E(2/2) F(3/3)\n";
    bool ok = true;
    for (int mode = 0; mode < 2; ++mode) {
        auto reqs = base;
        Trace trace;
        trace.enabled = true;
        const RunStats stats = mode == 0 ? runStatic(reqs, unit, 3, 2, &trace) : runContinuous(reqs, unit, 3, &trace);
        std::cout << (mode == 0 ? "\n  静态攒批：" : "\n  连续攒批：") << "\n";
        for (std::size_t i = 0; i < trace.rows.size(); ++i) std::cout << "    位置 " << i << " |" << trace.rows[i] << "|\n";
        std::cout << "    全部完成于第 " << stats.endUs << " 步；完成时刻：";
        for (const auto& r : reqs) std::cout << letterOf(r.id) << "=" << r.finishUs << " ";
        std::cout << "；白算 " << stats.wastedSlots << " 格\n";
        ok = ok && consistent(reqs);
    }
    return ok;
}

// ---------- 第二部分：两种负载下的对比 ----------

struct Comparison {
    Metrics sequential, fixedBatch, continuous;
    bool ok = true;
};

Comparison compare(const char* title, int count, Us meanGapUs, int maxBatch, Us timeoutUs) {
    const CostModel cost;
    const auto workload = makeWorkload(count, meanGapUs, 2026);
    Comparison c;

    std::printf("\n%s：%d 个请求，平均每 %.2f 秒到达一个；批大小上限 %d，静态攒批最多等 %.1f 秒\n", title, count,
                static_cast<double>(meanGapUs) / 1e6, maxBatch, static_cast<double>(timeoutUs) / 1e6);
    std::printf("            token/s  TTFT P50  TTFT P99  总延时 P50  总延时 P99   平均批     白算\n");

    auto a = workload;
    const RunStats sa = runContinuous(a, cost, 1);
    c.sequential = summarize(a, sa);
    printRow("逐条处理", c.sequential);

    auto b = workload;
    const RunStats sb = runStatic(b, cost, maxBatch, timeoutUs);
    c.fixedBatch = summarize(b, sb);
    printRow("静态攒批", c.fixedBatch);

    auto d = workload;
    const RunStats sd = runContinuous(d, cost, maxBatch);
    c.continuous = summarize(d, sd);
    printRow("连续攒批", c.continuous);

    c.ok = consistent(a) && consistent(b) && consistent(d) && sd.wastedSlots == 0;
    return c;
}

int main() {
    bool ok = toyExample();

    std::cout << "\n成本模型（假设）：一步 = 20 ms 固定开销 + 每条解码中的序列 0.5 ms + 每个 Prefill 的提示词 token 0.02 ms\n"
              << "时间单位：秒。TTFT = 首字延时；总延时 = 从到达到最后一个 token；P50 = 中位数\n";

    const Comparison light = compare("轻负载", 200, 4000000, 16, 500000);
    const Comparison heavy = compare("重负载", 200, 150000, 16, 500000);
    ok = ok && light.ok && heavy.ok;

    // 检查几条“应该成立”的结论。参数改得很极端时它们可能不成立，那时请想一想为什么。
    ok = ok && heavy.continuous.tokensPerSec > heavy.fixedBatch.tokensPerSec;
    ok = ok && heavy.fixedBatch.tokensPerSec > heavy.sequential.tokensPerSec;
    ok = ok && heavy.continuous.ttftP99 < heavy.fixedBatch.ttftP99;
    ok = ok && light.continuous.ttftP50 < light.fixedBatch.ttftP50;

    std::cout << (ok ? "\n自检通过\n" : "\n自检失败\n");
    return ok ? 0 : 1;
}
