// 单元测试辅助宏与工具函数。
//
// 提供轻量测试断言宏，用于非 Google Test 的独立测试文件。
// 当前测试文件使用 Google Test (gtest/gtest.h)，
// 本文件供未来纯 C++ 测试或快速开发验证使用。

#pragma once

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace wavedb {
namespace test {

// 从 Status 或 Result<T> 提取错误消息字符串。
inline std::string ErrMsg(const Status& s) { return s.message(); }

template <typename T>
std::string ErrMsg(const Result<T>& r) {
    return r.status.message();
}

}  // namespace test
}  // namespace wavedb

// 条件断言：失败时打印文件名:行号并 exit(1)。
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " — " #cond << "\n"; \
            std::exit(1);                                                               \
        }                                                                               \
    } while (0)

// 相等断言：失败时打印双方值。
#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        auto _va = (a);                                                                                  \
        auto _vb = (b);                                                                                  \
        if (_va != _vb) {                                                                                \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " — " #a << " != " << #b << "  (" << _va \
                      << " != " << _vb << ")\n";                                                         \
            std::exit(1);                                                                                \
        }                                                                                                \
    } while (0)

// Status/Result 断言：失败时打印错误码和消息。
#define CHECK_OK(expr)                                                         \
    do {                                                                       \
        auto&& _r = (expr);                                                    \
        if (!_r.ok()) {                                                        \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " — " #expr \
                      << " error: " << ::wavedb::test::ErrMsg(_r) << "\n";     \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

// 自动生成 main() 入口，调用 run_tests() 函数。
#define RUN_TESTS()                                  \
    int main() {                                     \
        std::cout << "=== " << __FILE__ << " ===\n"; \
        run_tests();                                 \
        std::cout << "PASS\n";                       \
        return 0;                                    \
    }

// 在 /tmp 下创建随机命名的临时目录（通过 mkdtemp）。
inline std::string MakeTempDir(const char* prefix) {
    std::string path = std::string("/tmp/") + prefix + "_XXXXXX";
    char* buf = &path[0];
    if (::mkdtemp(buf) == nullptr) {
        std::cerr << "mkdtemp failed\n";
        std::exit(1);
    }
    return path;
}

// 递归删除目录（测试后清理）。使用 std::filesystem 避免 shell 注入。
inline void RemoveDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}
