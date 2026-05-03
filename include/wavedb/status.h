#pragma once

#include <cstdint>
#include <string>

namespace wavedb {

enum class StatusCode : uint8_t {
    kOk = 0,
    kNotFound,
    kAlreadyExists,
    kInvalidArgument,
    kParseError,
    kIOError,
    kInternal,
};

class Status {
  public:
    Status() = default;
    Status(StatusCode code, std::string msg) : code_(code), msg_(std::move(msg)) {}

    static Status OK() { return Status{StatusCode::kOk, {}}; }

    bool ok() const { return code_ == StatusCode::kOk; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return msg_; }

    // 允许 if (!status) 判断
    explicit operator bool() const { return ok(); }

  private:
    StatusCode code_ = StatusCode::kOk;
    std::string msg_;
};

template <typename T>
struct Result {
    T value{};
    Status status;

    Result() = default;

    // 从值构造（隐式，方便 return 值）
    // NOLINTNEXTLINE(google-explicit-constructor)
    Result(T v) : value(std::move(v)), status(Status::OK()) {}

    // 从 Status 构造（隐式，方便 return 错误）
    // NOLINTNEXTLINE(google-explicit-constructor)
    Result(Status s) : value{}, status(std::move(s)) {}

    bool ok() const { return status.ok(); }
    explicit operator bool() const { return ok(); }

    T& operator*() { return value; }
    const T& operator*() const { return value; }
    T* operator->() { return &value; }
    const T* operator->() const { return &value; }
};

}  // namespace wavedb
