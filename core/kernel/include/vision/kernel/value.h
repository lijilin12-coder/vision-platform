#pragma once

#include <optional>
#include <string>
#include <variant>

#include "vision/kernel/geometry.h"
#include "vision/kernel/image.h"

namespace vision {

// 端口类型（术语表 §2）。连线两端的端口类型必须兼容（不变量 R2）。
enum class PortType { Image, Region, Blobs, Number, Bool, Text, Judgement };

const char* toString(PortType type);
std::optional<PortType> parsePortType(const std::string& text);

// POC 阶段：类型完全相同才兼容。以后可以在这里加入隐式转换规则。
bool isCompatible(PortType from, PortType to);

// 端口上流动的数据。
using Value = std::variant<std::monostate, Image, Region, BlobList, double, bool, std::string, Judgement>;

// 值的实际类型；monostate 返回空。
std::optional<PortType> typeOf(const Value& value);

}  // namespace vision
