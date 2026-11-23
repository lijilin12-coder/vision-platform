#pragma once

// BC-1 算子目录的公开接口。其他上下文、算子、适配器只能包含这个头文件。

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/result.h"
#include "vision/kernel/geometry.h"
#include "vision/kernel/match_template.h"
#include "vision/kernel/signature.h"
#include "vision/kernel/value.h"

namespace vision::catalog {

// 算子元数据（发布语言）：签名 + 展示信息。
struct OperatorMeta {
    OperatorSignature signature;
    std::string displayName;
    std::string category;
    std::string description;
};

// 算子执行时的上下文：读取输入、参数、ROI，写入输出。
class OpContext {
public:
    // templates：参数名 → 已经解码好的匹配模板。没有模板参数的算子不用管它。
    OpContext(const std::map<std::string, Value>& inputs, const ParamMap& params, std::optional<Roi> roi,
              const std::map<std::string, MatchTemplateData>* templates = nullptr)
        : inputs_(inputs), params_(params), roi_(std::move(roi)), templates_(templates) {}

    // 读取某个输入；类型不对时返回错误。
    template <typename T>
    Result<T> input(const std::string& name) const {
        auto it = inputs_.find(name);
        if (it == inputs_.end() || !std::holds_alternative<T>(it->second)) {
            return Result<T>::failure("operator.input", "输入 " + name + " 缺失或类型不对");
        }
        return std::get<T>(it->second);
    }

    double number(const std::string& name) const { return std::get<double>(params_.at(name)); }
    bool flag(const std::string& name) const { return std::get<bool>(params_.at(name)); }
    const std::string& text(const std::string& name) const { return std::get<std::string>(params_.at(name)); }
    const std::optional<Roi>& roi() const { return roi_; }

    // 取模板参数对应的匹配模板。算子拿到的是图像，不用关心它存在哪里。
    Result<MatchTemplateData> matchTemplate(const std::string& param) const {
        if (templates_ != nullptr) {
            if (const auto it = templates_->find(param); it != templates_->end()) return it->second;
        }
        return Result<MatchTemplateData>::failure("operator.template", "模板参数 " + param + " 还没有设置");
    }

    void setOutput(const std::string& name, Value value) { outputs_[name] = std::move(value); }
    std::map<std::string, Value>& outputs() { return outputs_; }

private:
    const std::map<std::string, Value>& inputs_;
    const ParamMap& params_;
    std::optional<Roi> roi_;
    const std::map<std::string, MatchTemplateData>* templates_;
    std::map<std::string, Value> outputs_;
};

// 算子：一种算法能力（策略模式）。算子属于通用域的封装，不使用 DDD 战术模式。
class Operator {
public:
    virtual ~Operator() = default;
    virtual Status execute(OpContext& ctx) = 0;
};

using OperatorFactory = std::function<std::unique_ptr<Operator>()>;

// 应用服务：算子目录（开放主机服务）。
class OperatorCatalog {
public:
    // 进程内唯一的目录，静态注册的算子都登记在这里。
    static OperatorCatalog& global();

    // 注册算子；违反签名规则或 ID 冲突时返回错误（事件 A1 / A2）。
    Status registerOperator(OperatorMeta meta, OperatorFactory factory);

    // 供 VISION_REGISTER_OPERATOR 在静态初始化时调用：失败不抛异常，而是记录下来。
    bool registerStatic(OperatorMeta meta, OperatorFactory factory);
    const std::vector<Error>& registrationErrors() const { return registrationErrors_; }

    std::vector<OperatorMeta> list() const;
    std::optional<OperatorMeta> find(const std::string& id) const;

    // 创建算子实例；主版本号不匹配时返回错误（D5-1）。
    Result<std::unique_ptr<Operator>> create(const std::string& id, int majorVersion) const;

private:
    struct Entry {
        OperatorMeta meta;
        OperatorFactory factory;
    };
    std::map<std::string, Entry> entries_;
    std::vector<Error> registrationErrors_;
};

}  // namespace vision::catalog

// 一行注册一个算子。算子类需要提供 static vision::catalog::OperatorMeta meta();
#define VISION_REGISTER_OPERATOR(OpClass)                                                           \
    namespace {                                                                                     \
    [[maybe_unused]] const bool vision_registered_##OpClass =                                       \
        ::vision::catalog::OperatorCatalog::global().registerStatic(                                \
            OpClass::meta(), [] { return std::unique_ptr<::vision::catalog::Operator>(new OpClass()); }); \
    }
