// WaveDB 错误处理基础设施。
//
// 设计原则：
//   使用 Status + Result<T> 而非 C++ exception。
//   原因：(1) 错误路径可预测，无 hidden control flow (2) 热路径无 unwind 开销
//         (3) 错误码便于序列化/日志记录。
//
// Status 是轻量栈对象（16 字节：code_ + string msg_），
// 成功时不分配堆内存（msg_ 为空 string，SSO 覆盖）。
// 失败时的堆分配（string）在错误路径上可接受——错误路径不是热路径。
//
// Result<T> 同时承载值和错误，用法类似 Rust Result。
// 隐式构造允许 return 值或 return Status，减少样板代码。

#pragma once

#include <cstdint>
#include <string>

namespace wavedb {

// 错误码枚举。uint8_t 存储，保持 Status 紧凑。
enum class StatusCode : uint8_t {
    OK = 0,
    NOT_FOUND,         // 表/列/Part 不存在
    ALREADY_EXISTS,    // 重复创建表
    INVALID_ARGUMENT,  // 参数类型/数量不匹配
    PARSE_ERROR,       // SQL 或 JSON 解析失败
    IO_ERROR,          // 文件读写/创建失败
    INTERNAL,         // 内部逻辑错误（不应发生）
};

// 操作状态。ok() 表示成功，否则包含错误码和描述信息。
class Status {
  public:
    Status() = default;
    Status(StatusCode code, std::string msg) : code_(code), msg_(std::move(msg)) {}

    static Status OK() { return Status{StatusCode::OK, {}}; }

    bool ok() const { return code_ == StatusCode::OK; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return msg_; }

    // 允许 if (!status) 进行错误判断。
    explicit operator bool() const { return ok(); }

  private:
    StatusCode code_ = StatusCode::OK;
    std::string msg_;  // 成功时为空（SSO），失败时包含描述
};

// 结果或错误。
// 隐式构造支持两种用法：
//   return my_value;          // → Result<T> 包含值
//   return Status(...);        // → Result<T> 包含错误
template <typename T>
struct Result {
    T value{};
    Status status;

    Result() = default;

    // 从值隐式构造（便利 return 表达式）。
    // NOLINTNEXTLINE(google-explicit-constructor)
    Result(T v) : value(std::move(v)), status(Status::OK()) {}

    // 从 Status 隐式构造（便利 return 错误）。
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
