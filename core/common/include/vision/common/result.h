#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace vision {

// 命令被拒绝时返回的错误。注意：被拒绝不是领域事件（docs/ddd/05-tactical-design.md §0）。
struct Error {
    std::string code;     // 机器可读，例如 "authoring.cycle"
    std::string message;  // 给人看的说明
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : data_(std::move(value)) {}            // NOLINT：允许隐式转换，写法更简洁
    Result(Error error) : data_(std::move(error)) {}        // NOLINT

    static Result failure(std::string code, std::string message) {
        return Result(Error{std::move(code), std::move(message)});
    }

    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }

    const T& value() const& {
        ensureOk();
        return std::get<T>(data_);
    }
    T& value() & {
        ensureOk();
        return std::get<T>(data_);
    }
    T&& value() && {
        ensureOk();
        return std::get<T>(std::move(data_));
    }

    const Error& error() const { return std::get<Error>(data_); }

private:
    void ensureOk() const {
        if (!ok()) throw std::logic_error("Result 中没有值：" + error().message);
    }
    std::variant<T, Error> data_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}  // NOLINT

    static Result success() { return Result(); }
    static Result failure(std::string code, std::string message) {
        return Result(Error{std::move(code), std::move(message)});
    }

    bool ok() const { return ok_; }
    explicit operator bool() const { return ok_; }
    const Error& error() const { return error_; }

private:
    Error error_{};
    bool ok_ = true;
};

using Status = Result<void>;

}  // namespace vision
