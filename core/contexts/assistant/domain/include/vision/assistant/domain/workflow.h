#pragma once

#include <string>
#include <vector>

#include "vision/common/result.h"
#include "vision/kernel/signature.h"

namespace vision::assistant {

// ---------- 提议 ----------

// 顾问想对候选方案做的一处修改。
// “工作流内调参”时只允许 SetParam，而且只能改工作流声明的可调项（A10）。
// 其余几种是改变结构的修改，留给以后的“自由搭建”；现在出现就会被拒绝。
struct ProposedChange {
    enum class Kind { SetParam, SetRoi, AddNode, RemoveNode, Connect, Disconnect };
    Kind kind = Kind::SetParam;
    std::string nodeKey;  // 工作流里节点的名字，例如 "bin"
    std::string param;    // 参数名，例如 "value"
    double value = 0;
};

// 值对象：提议。顾问（大模型是它的一种实现）每一轮只给出这样一份数据，
// 由我们的代码检查之后才会生效（docs/ddd/07-recipe-assistant.md §3）。
struct Proposal {
    std::vector<ProposedChange> changes;
    std::string rationale;  // 理由
    bool finish = false;    // 顾问认为不必再改了
};

// ---------- 工作流 ----------

struct WorkflowNode {
    std::string key;  // 在工作流里的名字
    std::string operatorId;
    ParamMap params;  // 初始参数
};

struct WorkflowEdge {
    std::string from;  // "gray.image"
    std::string to;    // "bin.image"
};

// 值对象：可调项。工作流里声明为可以由顾问修改的一个参数，带取值范围。
struct Tunable {
    std::string nodeKey;
    std::string param;
    double min = 0;
    double max = 0;
    double initial = 0;
    std::string hint;  // 给顾问看的说明：这个参数是什么意思、怎么调

    std::string name() const { return nodeKey + "." + param; }
};

// 值对象：稳健性检查的设定。对图像做已知的扰动之后重新评估。
struct RobustnessProbe {
    enum class Kind { Brightness, Noise };
    Kind kind = Kind::Brightness;
    std::vector<double> levels;  // 亮度：乘的倍数；噪声：标准差
};
const char* toString(RobustnessProbe::Kind kind);

// 值对象：模板槽。工作流里声明“这个节点的这个参数需要一个匹配模板”。
// 工作流是通用的，不含任何具体产品的图像；模板由任务书提供（docs/ddd/07-recipe-assistant.md §8.5）。
struct TemplateSlot {
    std::string name;      // 槽的名字，任务书按它提供模板，例如 "locator"
    std::string nodeKey;   // 哪个节点
    std::string param;     // 它的哪个模板参数
    std::string source;    // 模板从哪个输出端口的图像上框，例如 "gray.image"：
                           // 模板必须取自“这个节点实际看到的图像”（经过了前面的预处理），而不是相机的原图
    std::string hint;      // 给人看的说明：应该框什么
};

// 应用一处可调项的修改时，要对候选方案执行的编辑
struct TunableEdit {
    std::string nodeKey;
    std::string param;
    double value = 0;
};

// 聚合根：工作流（Workflow）。针对一类检测任务的、可复用的方案骨架：
// 固定的节点和连线，声明哪些参数可调及其范围，以及要做哪些稳健性检查。
// 工作流不是方案：方案是针对某个具体产品配好参数、可以发布的检测配置。
//
// 不变量：
//   - 节点的名字不重复；连线两端的节点都存在
//   - 可调项指向存在的节点，不重复，并且 min ≤ initial ≤ max
//   - 模板槽的名字不重复；它指向的节点、它的图像来源都存在；模板参数不能同时是可调项（模板由人定，顾问不能改）
class Workflow {
public:
    static Result<Workflow> create(std::string id, int version, std::string name, std::string description,
                                   std::vector<WorkflowNode> nodes, std::vector<WorkflowEdge> edges,
                                   std::vector<Tunable> tunables, std::vector<RobustnessProbe> probes,
                                   std::vector<TemplateSlot> templateSlots = {});

    // A10：工作流内调参时，提议只能修改声明的可调项，且在声明的范围内。
    // 不合法时返回失败，错误信息说明原因（会告诉顾问）。
    Status validate(const Proposal& proposal) const;

    const std::string& id() const { return id_; }
    int version() const { return version_; }
    const std::string& name() const { return name_; }
    const std::string& description() const { return description_; }
    const std::vector<WorkflowNode>& nodes() const { return nodes_; }
    const std::vector<WorkflowEdge>& edges() const { return edges_; }
    const std::vector<Tunable>& tunables() const { return tunables_; }
    const std::vector<RobustnessProbe>& probes() const { return probes_; }
    const std::vector<TemplateSlot>& templateSlots() const { return templateSlots_; }
    const Tunable* findTunable(const std::string& nodeKey, const std::string& param) const;

private:
    Workflow() = default;
    std::string id_;
    int version_ = 1;
    std::string name_;
    std::string description_;
    std::vector<WorkflowNode> nodes_;
    std::vector<WorkflowEdge> edges_;
    std::vector<Tunable> tunables_;
    std::vector<RobustnessProbe> probes_;
    std::vector<TemplateSlot> templateSlots_;
};

}  // namespace vision::assistant
