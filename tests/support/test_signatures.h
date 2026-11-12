#pragma once

// 测试用的算子签名。领域层测试不依赖真实算子（也就不依赖 OpenCV）。

#include "vision/kernel/signature.h"

namespace vision::test {

inline OperatorSignature imageInput() {
    OperatorSignature s;
    s.id = kImageInputOperatorId;
    s.outputs = {{"image", PortType::Image, false}};
    return s;
}

inline OperatorSignature gray() {
    OperatorSignature s;
    s.id = "test.gray";
    s.inputs = {{"image", PortType::Image, false}};
    s.outputs = {{"image", PortType::Image, false}};
    return s;
}

inline OperatorSignature threshold() {
    OperatorSignature s;
    s.id = "test.threshold";
    s.inputs = {{"image", PortType::Image, false}};
    s.outputs = {{"region", PortType::Region, false}};
    s.params = {{"value", ParamType::Number, 128.0, 0.0, 255.0, {}, "阈值"},
                {"method", ParamType::Choice, std::string("fixed"), std::nullopt, std::nullopt, {"fixed", "otsu"}, "方法"}};
    return s;
}

inline OperatorSignature blob() {
    OperatorSignature s;
    s.id = "test.blob";
    s.inputs = {{"region", PortType::Region, false}};
    s.outputs = {{"blobs", PortType::Blobs, false}};
    return s;
}

inline OperatorSignature judge() {
    OperatorSignature s;
    s.id = "test.judge";
    s.inputs = {{"blobs", PortType::Blobs, false}};
    s.outputs = {{"judgement", PortType::Judgement, false}};
    return s;
}

}  // namespace vision::test
