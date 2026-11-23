#pragma once

#include <optional>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"
#include "vision/common/result.h"

namespace vision::data {

// 样本的 ID 就是图像内容的哈希（SHA-256）：同一张图像换个文件名再导入，仍然是同一个样本（规则 N1 靠它）。
using SampleId = std::string;

// 图像级的标签：人确认的“这件产品合格还是不合格”
enum class Label { Unlabeled, Ok, Ng };
const char* toString(Label label);

// 样本用途：数据的第一层划分（docs/ddd/08-platforms-and-training.md §3）
enum class SampleUse {
    Unassigned,  // 还没有决定
    Benchmark,   // 留作基准：进了某个已冻结的基准图集，用来评价方案
    Training,    // 用于训练：进了某个已冻结的数据集版本
};
const char* toString(SampleUse use);

// 聚合根：样本。一幅带标签的图像。
//
// 不变量：
//   N1  一个样本要么留作基准，要么用于训练，不能两者都是。
//       用途一旦定成其中一种，就不能再改成另一种（也不能改回“未分配”）：
//       已冻结的基准图集和数据集版本都不可修改，它们里面的样本用途也就永远成立。
//   -   用途已经确定的样本不能改标签：冻结的图集和数据集版本记录的是冻结时的标签，改了就对不上了。
class Sample : public EventRecorder {
public:
    static Result<Sample> import(SampleId contentHash, long long bytes, std::string originalName);
    static Sample restore(SampleId id, long long bytes, std::vector<std::string> names, Label label, SampleUse use);

    // 同样的内容又导入了一次（文件名可能不同）：只是多记一个名字
    void seenAs(const std::string& name);
    Status setLabel(Label label);
    // 把用途定为留作基准 / 用于训练（N1）。已经是这个用途时什么都不做。
    Status reserveFor(SampleUse use);

    const SampleId& id() const { return id_; }
    long long bytes() const { return bytes_; }
    const std::vector<std::string>& names() const { return names_; }
    Label label() const { return label_; }
    SampleUse use() const { return use_; }

private:
    Sample() = default;
    SampleId id_;
    long long bytes_ = 0;
    std::vector<std::string> names_;  // 导入时用过的文件名，只作显示
    Label label_ = Label::Unlabeled;
    SampleUse use_ = SampleUse::Unassigned;
};

struct SampleImported final : DomainEvent {
    std::string sampleId;
    explicit SampleImported(std::string id) : sampleId(std::move(id)) {}
    const char* name() const override { return "SampleImported"; }
};

class SampleRepository {
public:
    virtual ~SampleRepository() = default;
    virtual Status save(const Sample& sample) = 0;
    virtual std::optional<Sample> find(const SampleId& id) = 0;
    virtual std::vector<SampleId> listIds() = 0;
};

}  // namespace vision::data
