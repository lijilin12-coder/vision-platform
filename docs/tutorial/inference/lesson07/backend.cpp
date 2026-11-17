// 推理后端：一个接口，两种实现；以及推理引擎内部的“子图划分 + CPU 回退”。
//
// 这个示例里没有真的推理引擎，也没有真的 NPU，全是模拟的。它演示的是结构：
//
//   第一层（项目要写的）：InferBackend 接口。平台核心只认识这个接口，
//                         开发机上接 ONNX Runtime，鸿蒙上接 MindSpore Lite。
//   第二层（推理引擎内部）：一个模型交给引擎之后，引擎检查每个算子在加速硬件上是否支持，
//                         把连续受支持的算子划成一段交给 NPU，剩下的回退到 CPU。
//
//   macOS / Linux：clang++ -std=c++17 -I../common backend.cpp -o backend && ./backend
//   Windows：      cl /std:c++17 /EHsc /utf-8 /I..\common backend.cpp && backend.exe
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "inftut/common.h"

using Tensor = std::vector<float>;

// ====================================================================================
// 一个极简的“模型”：一串算子，前一个的输出是后一个的输入。
// 真实的模型是一张图（第 2 课），这里用一条链就足够说明划分的问题。
// ====================================================================================

enum class Op { Conv, BatchNorm, Relu, HardSwish, Pool, Gemm };

const char* opName(Op op) {
    switch (op) {
        case Op::Conv: return "Conv";
        case Op::BatchNorm: return "BatchNorm";
        case Op::Relu: return "Relu";
        case Op::HardSwish: return "HardSwish";
        case Op::Pool: return "Pool";
        case Op::Gemm: return "Gemm";
    }
    return "?";
}

struct Model {
    std::string name;
    std::vector<Op> nodes;
};

// 每个算子的“标准答案”：用最普通的 float 计算。数值上没有任何实际意义，只要是确定的函数就行。
Tensor referenceKernel(Op op, const Tensor& in) {
    Tensor out(in.size());
    const std::size_t n = in.size();
    for (std::size_t i = 0; i < n; ++i) {
        const float x = in[i];
        switch (op) {
            case Op::Conv: out[i] = 0.25f * in[(i + n - 1) % n] + 0.5f * x + 0.25f * in[(i + 1) % n] + 0.01f; break;
            case Op::BatchNorm: out[i] = 1.7f * x - 0.3f; break;
            case Op::Relu: out[i] = std::max(x, 0.0f); break;
            case Op::HardSwish: out[i] = x * std::min(std::max(x + 3.0f, 0.0f), 6.0f) / 6.0f; break;
            case Op::Pool: out[i] = std::max(x, in[(i + 1) % n]); break;
            case Op::Gemm: out[i] = 0.9f * x + 0.1f * in[(i * 7 + 3) % n]; break;
        }
    }
    return out;
}

// ====================================================================================
// 第二层：推理引擎内部。设备（Device）+ 划分（partition）
// ====================================================================================

// 一种计算设备。ONNX Runtime 里叫 execution provider，MindSpore Lite 里是 Context 里的 DeviceInfo。
class Device {
public:
    virtual ~Device() = default;
    virtual const char* name() const = 0;
    virtual bool supports(Op op) const = 0;
    virtual Tensor run(Op op, const Tensor& in) const = 0;
    virtual double costPerNode() const = 0;  // 模拟的耗时（毫秒），只用来估算
};

class CpuDevice final : public Device {
public:
    const char* name() const override { return "CPU"; }
    bool supports(Op) const override { return true; }  // CPU 支持全部算子：它是“兜底”的
    Tensor run(Op op, const Tensor& in) const override { return referenceKernel(op, in); }
    double costPerNode() const override { return 5.0; }
};

// 模拟的 NPU：快，但只支持一部分算子，而且用半精度（FP16）计算，结果和 CPU 略有不同。
class NpuDevice final : public Device {
public:
    explicit NpuDevice(std::set<Op> supported) : supported_(std::move(supported)) {}
    const char* name() const override { return "NPU"; }
    bool supports(Op op) const override { return supported_.count(op) > 0; }
    Tensor run(Op op, const Tensor& in) const override {
        Tensor out = referenceKernel(op, in);
        for (float& v : out) v = toHalfPrecision(v);
        return out;
    }
    double costPerNode() const override { return 1.0; }

private:
    // FP16 的尾数只有 10 位（FP32 有 23 位）。这里把尾数截到 10 位来模拟它的精度。
    static float toHalfPrecision(float v) {
        if (v == 0.0f) return v;
        int exponent = 0;
        const float mantissa = std::frexp(v, &exponent);                 // v = mantissa × 2^exponent，0.5 ≤ |mantissa| < 1
        return std::ldexp(std::round(mantissa * 2048.0f) / 2048.0f, exponent);
    }
    std::set<Op> supported_;
};

// 一段连续的、放在同一个设备上的算子。
struct Segment {
    const Device* device = nullptr;
    std::size_t begin = 0, end = 0;  // 模型里第 [begin, end) 个算子
};

// 划分：从头扫一遍，加速设备支持的算子给加速设备，不支持的回退到 CPU；相邻且设备相同的合并成一段。
std::vector<Segment> partition(const Model& model, const Device* accelerator, const Device& cpu) {
    std::vector<Segment> segments;
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        const Device* device = (accelerator != nullptr && accelerator->supports(model.nodes[i])) ? accelerator : &cpu;
        if (!segments.empty() && segments.back().device == device) {
            segments.back().end = i + 1;
        } else {
            segments.push_back(Segment{device, i, i + 1});
        }
    }
    return segments;
}

// 每在 CPU 和 NPU 之间切换一次，数据要在两种内存之间拷贝、转换格式。模拟的耗时（毫秒）。
constexpr double kSwitchCostMs = 12.0;

// ====================================================================================
// 第一层：项目里的接口。平台核心只依赖它，不知道后面是 ONNX Runtime 还是 MindSpore Lite。
// ====================================================================================

struct InferStats {
    std::size_t segments = 0;      // 模型被切成了几段
    std::size_t switches = 0;      // 一次推理里设备切换了几次
    std::size_t fallbackNodes = 0; // 有几个算子回退到了 CPU
    double estimatedMs = 0;        // 按上面的模拟耗时估算的一次推理时间
};

class InferBackend {
public:
    virtual ~InferBackend() = default;
    virtual std::string name() const = 0;
    // 加载模型。失败时返回 false，原因写进 error。真实的接口里参数是模型文件的路径。
    virtual bool load(const Model& model, std::string* error) = 0;
    // 推理一次。必须先 load 成功。
    virtual Tensor infer(const Tensor& input) = 0;
    virtual InferStats stats() const = 0;
};

// 用上面的“引擎”实现接口。accelerator 为空就是纯 CPU 后端。
class EngineBackend final : public InferBackend {
public:
    EngineBackend(std::string name, std::unique_ptr<Device> accelerator, bool allowFallback)
        : name_(std::move(name)), accelerator_(std::move(accelerator)), allowFallback_(allowFallback) {}

    std::string name() const override { return name_; }

    bool load(const Model& model, std::string* error) override {
        std::vector<Segment> segments = partition(model, accelerator_.get(), cpu_);
        InferStats stats;
        stats.segments = segments.size();
        stats.switches = segments.size() - 1;  // 相邻两段的设备一定不同，所以切换次数 = 段数 − 1
        for (const Segment& s : segments) {
            const std::size_t count = s.end - s.begin;
            stats.estimatedMs += static_cast<double>(count) * s.device->costPerNode();
            if (accelerator_ && s.device == &cpu_) stats.fallbackNodes += count;
        }
        stats.estimatedMs += static_cast<double>(stats.switches) * kSwitchCostMs;

        if (stats.fallbackNodes > 0 && !allowFallback_) {
            // 有的部署要求“必须全部跑在 NPU 上”：宁可加载失败，也不要悄悄变慢
            if (error != nullptr) {
                *error = "有 " + std::to_string(stats.fallbackNodes) + " 个算子 " + accelerator_->name() + " 不支持：";
                for (const Segment& s : segments)
                    if (s.device == &cpu_)
                        for (std::size_t i = s.begin; i < s.end; ++i) *error += std::string(opName(model.nodes[i])) + " ";
            }
            return false;
        }
        model_ = model;
        segments_ = std::move(segments);
        stats_ = stats;
        compiled_ = false;
        return true;
    }

    Tensor infer(const Tensor& input) override {
        if (!compiled_) {
            // 很多引擎把一部分准备工作推迟到第一次推理：给硬件编译子图、申请内存、把权重搬到设备上。
            // 这里用 sleep 模拟。它就是“第一次推理特别慢”的来源，也是需要预热的原因。
            std::this_thread::sleep_for(std::chrono::milliseconds(accelerator_ ? 30 : 5));
            compiled_ = true;
        }
        Tensor value = input;
        for (const Segment& s : segments_)
            for (std::size_t i = s.begin; i < s.end; ++i) value = s.device->run(model_.nodes[i], value);
        return value;
    }

    InferStats stats() const override { return stats_; }

    // 打印划分结果
    void printPlan() const {
        for (const Segment& s : segments_) {
            std::printf("      [%s]", s.device->name());
            for (std::size_t i = s.begin; i < s.end; ++i) std::printf(" %s", opName(model_.nodes[i]));
            std::printf("\n");
        }
    }

private:
    std::string name_;
    CpuDevice cpu_;
    std::unique_ptr<Device> accelerator_;
    bool allowFallback_;
    Model model_;
    std::vector<Segment> segments_;
    InferStats stats_;
    bool compiled_ = false;
};

// ====================================================================================
// 演示
// ====================================================================================

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("  [失败] %s\n", what);
        ++failures;
    }
}

std::unique_ptr<Device> makeNpu() {
    // 这个假想的 NPU 不支持 HardSwish
    return std::make_unique<NpuDevice>(std::set<Op>{Op::Conv, Op::BatchNorm, Op::Relu, Op::Pool, Op::Gemm});
}

Tensor makeInput() {
    inftut::Rng rng(42);
    Tensor input(256);
    for (float& v : input) v = rng.uniform(-1.0f, 1.0f);
    return input;
}

// 一致性验证（POC-4 的做法）：同一个输入分别送进两个后端，比较输出的最大误差。
double compare(InferBackend& a, InferBackend& b, const Tensor& input) { return inftut::maxAbsDiff(a.infer(input), b.infer(input)); }

void demo(const Model& model, std::size_t expectedSwitches) {
    std::printf("模型 %s（%zu 个算子）\n", model.name.c_str(), model.nodes.size());
    EngineBackend cpu("纯 CPU 后端", nullptr, true);
    EngineBackend npu("NPU + CPU 回退", makeNpu(), true);
    std::string error;
    expect(cpu.load(model, &error), "CPU 后端应该能加载任何模型");
    expect(npu.load(model, &error), "允许回退时，NPU 后端应该能加载");

    for (EngineBackend* backend : {&cpu, &npu}) {
        const InferStats s = backend->stats();
        std::printf("  %s：切成 %zu 段，切换 %zu 次，%zu 个算子回退到 CPU，估算 %.0f ms\n", backend->name().c_str(),
                    s.segments, s.switches, s.fallbackNodes, s.estimatedMs);
        backend->printPlan();
    }
    expect(npu.stats().switches == expectedSwitches, "切换次数和预期不符");

    const double diff = compare(cpu, npu, makeInput());
    std::printf("  一致性：两个后端输出的最大误差 %.2e，%s（阈值 1e-3）\n\n", diff, diff < 1e-3 ? "通过" : "不通过");
    expect(diff < 1e-3, "FP16 的误差应该在 1e-3 以内");
    expect(diff > 0, "两个后端的结果不应该逐位相同（NPU 用的是半精度）");
}

}  // namespace

int main() {
    // 模型 A：全部算子 NPU 都支持
    const Model friendly{"A（全部受支持）",
                         {Op::Conv, Op::BatchNorm, Op::Relu, Op::Conv, Op::BatchNorm, Op::Relu, Op::Pool, Op::Gemm}};
    // 模型 B：把激活函数换成了 HardSwish，NPU 不支持
    const Model hostile{"B（激活函数是 HardSwish）",
                        {Op::Conv, Op::BatchNorm, Op::HardSwish, Op::Conv, Op::BatchNorm, Op::HardSwish, Op::Pool, Op::Gemm}};

    demo(friendly, 0);
    demo(hostile, 4);

    // 不允许回退：加载模型 B 应该失败，并说清楚是哪些算子
    {
        EngineBackend strict("NPU（不允许回退）", makeNpu(), false);
        std::string error;
        const bool loaded = strict.load(hostile, &error);
        std::printf("不允许回退时加载模型 B：%s\n  %s\n\n", loaded ? "成功" : "失败", error.c_str());
        expect(!loaded && !error.empty(), "不允许回退时，含有不支持算子的模型应该加载失败并给出原因");
    }

    // 预热：第一次推理包含了推迟到这时才做的准备工作
    {
        EngineBackend npu("NPU + CPU 回退", makeNpu(), true);
        std::string error;
        expect(npu.load(friendly, &error), "加载");
        const Tensor input = makeInput();
        std::printf("预热：同一个模型连续推理 4 次的耗时\n ");
        double first = 0, last = 0;
        for (int i = 0; i < 4; ++i) {
            last = inftut::onceMs([&] { npu.infer(input); });
            if (i == 0) first = last;
            std::printf(" %.2f ms", last);
        }
        std::printf("\n\n");
        expect(first > last * 2, "第一次推理应该明显更慢");
    }

    if (failures > 0) {
        std::printf("有 %d 项检查没有通过\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("全部检查通过\n");
    return EXIT_SUCCESS;
}
