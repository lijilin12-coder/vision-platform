#include <algorithm>
#include <cmath>

#include "vision/kernel/geometry.h"
#include "vision/kernel/image.h"
#include "vision/kernel/signature.h"
#include "vision/kernel/value.h"

namespace vision {

// ---------- Image ----------

Result<Image> Image::create(int width, int height, Format format, std::vector<std::uint8_t> pixels) {
    if (width <= 0 || height <= 0) {
        return Result<Image>::failure("kernel.image.size", "图像宽高必须大于 0");
    }
    const std::size_t channels = format == Format::Gray8 ? 1 : 3;
    const std::size_t expected = static_cast<std::size_t>(width) * height * channels;
    if (pixels.size() != expected) {
        return Result<Image>::failure("kernel.image.pixels", "像素数量与宽高、通道数不一致");
    }
    Image img;
    img.width_ = width;
    img.height_ = height;
    img.format_ = format;
    img.pixels_ = std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
    return img;
}

// ---------- Roi / Region ----------

Result<Roi> Roi::rect(double x, double y, double width, double height) {
    if (!(width > 0) || !(height > 0)) {
        return Result<Roi>::failure("kernel.roi.size", "ROI 的宽高必须大于 0");
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return Result<Roi>::failure("kernel.roi.position", "ROI 的位置必须是有限数");
    }
    return Roi(x, y, width, height);
}

Result<Region> Region::fromMask(Image mask) {
    if (mask.empty() || mask.format() != Image::Format::Gray8) {
        return Result<Region>::failure("kernel.region.mask", "区域的掩码必须是非空的单通道图像");
    }
    return Region(std::move(mask));
}

// ---------- PortType / Value ----------

const char* toString(PortType type) {
    switch (type) {
        case PortType::Image: return "image";
        case PortType::Region: return "region";
        case PortType::Blobs: return "blobs";
        case PortType::Number: return "number";
        case PortType::Bool: return "bool";
        case PortType::Text: return "text";
        case PortType::Judgement: return "judgement";
        case PortType::Pose: return "pose";
    }
    return "unknown";
}

std::optional<PortType> parsePortType(const std::string& text) {
    for (auto t : {PortType::Image, PortType::Region, PortType::Blobs, PortType::Number, PortType::Bool,
                   PortType::Text, PortType::Judgement, PortType::Pose}) {
        if (text == toString(t)) return t;
    }
    return std::nullopt;
}

bool isCompatible(PortType from, PortType to) { return from == to; }

std::optional<PortType> typeOf(const Value& value) {
    switch (value.index()) {
        case 1: return PortType::Image;
        case 2: return PortType::Region;
        case 3: return PortType::Blobs;
        case 4: return PortType::Number;
        case 5: return PortType::Bool;
        case 6: return PortType::Text;
        case 7: return PortType::Judgement;
        case 8: return PortType::Pose;
        default: return std::nullopt;
    }
}

// ---------- ParamDef ----------

Status ParamDef::validate(const ParamValue& value) const {
    auto fail = [this](const std::string& why) {
        return Status::failure("kernel.param.invalid", "参数 " + name + " " + why);
    };
    switch (type) {
        case ParamType::Number: {
            if (!std::holds_alternative<double>(value)) return fail("必须是数值");
            const double v = std::get<double>(value);
            if (!std::isfinite(v)) return fail("必须是有限数");
            if (min && v < *min) return fail("不能小于 " + std::to_string(*min));
            if (max && v > *max) return fail("不能大于 " + std::to_string(*max));
            return Status::success();
        }
        case ParamType::Bool:
            return std::holds_alternative<bool>(value) ? Status::success() : fail("必须是布尔值");
        case ParamType::Template:
            return std::holds_alternative<std::string>(value) ? Status::success() : fail("必须是匹配模板的 ID");
        case ParamType::Model:
            return std::holds_alternative<std::string>(value) ? Status::success() : fail("必须是模型版本的 ID");
        case ParamType::Text:
            return std::holds_alternative<std::string>(value) ? Status::success() : fail("必须是文本");
        case ParamType::Choice: {
            if (!std::holds_alternative<std::string>(value)) return fail("必须是选项之一");
            const auto& v = std::get<std::string>(value);
            if (std::find(choices.begin(), choices.end(), v) == choices.end()) {
                return fail("不是有效选项：" + v);
            }
            return Status::success();
        }
    }
    return fail("类型未知");
}

// ---------- OperatorSignature ----------

namespace {
template <typename T>
const T* findByName(const std::vector<T>& items, const std::string& name) {
    auto it = std::find_if(items.begin(), items.end(), [&](const T& i) { return i.name == name; });
    return it == items.end() ? nullptr : &*it;
}
}  // namespace

const PortDef* OperatorSignature::findInput(const std::string& name) const { return findByName(inputs, name); }
const PortDef* OperatorSignature::findOutput(const std::string& name) const { return findByName(outputs, name); }
const ParamDef* OperatorSignature::findParam(const std::string& name) const { return findByName(params, name); }

bool OperatorSignature::contributesToVerdict() const {
    return std::any_of(outputs.begin(), outputs.end(),
                       [](const PortDef& p) { return p.type == PortType::Judgement; });
}

ParamMap OperatorSignature::defaultParams() const {
    ParamMap out;
    for (const auto& p : params) out[p.name] = p.defaultValue;
    return out;
}

}  // namespace vision
