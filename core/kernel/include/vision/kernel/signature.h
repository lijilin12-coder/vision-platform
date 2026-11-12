#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "vision/common/result.h"
#include "vision/kernel/value.h"

namespace vision {

// 参数值。
using ParamValue = std::variant<double, bool, std::string>;
using ParamMap = std::map<std::string, ParamValue>;

enum class ParamType { Number, Bool, Text, Choice };

// 参数定义（术语表 §2）：名称、类型、默认值、取值范围。
struct ParamDef {
    std::string name;
    ParamType type = ParamType::Number;
    ParamValue defaultValue = 0.0;
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::string> choices;  // 仅 Choice 类型
    std::string label;

    // 不变量 R4：参数值必须满足类型和范围。
    Status validate(const ParamValue& value) const;
};

// 端口定义。
struct PortDef {
    std::string name;
    PortType type = PortType::Image;
    bool optional = false;
};

// 算子签名：共享内核的一部分。算子目录、方案编排、检测执行三方都以它描述“一个算子长什么样”，
// 必须完全一致（docs/ddd/04-context-map.md 共享内核）。
struct OperatorSignature {
    std::string id;         // 例如 "vision.threshold"
    int majorVersion = 1;   // 主版本号表示兼容性（D5-1）
    std::vector<PortDef> inputs;
    std::vector<PortDef> outputs;
    std::vector<ParamDef> params;

    const PortDef* findInput(const std::string& name) const;
    const PortDef* findOutput(const std::string& name) const;
    const ParamDef* findParam(const std::string& name) const;

    // 是否为判定提供判定依据（有 Judgement 类型的输出）。
    bool contributesToVerdict() const;

    ParamMap defaultParams() const;
};

// 图像入口算子：检测时由执行引擎把图像直接送入它的输出。
inline constexpr const char* kImageInputOperatorId = "vision.image_input";

}  // namespace vision
