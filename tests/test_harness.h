#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

namespace wavedb {
namespace test {

inline std::string ErrMsg(const Status& s) { return s.message(); }

template <typename T>
std::string ErrMsg(const Result<T>& r) {
    return r.status.message();
}

}  // namespace test
}  // namespace wavedb

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " — " #cond << "\n"; \
            std::exit(1);                                                               \
        }                                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        auto _va = (a);                                                                                  \
        auto _vb = (b);                                                                                  \
        if (_va != _vb) {                                                                                \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " — " #a << " != " #b << "  (" << _va \
                      << " != " << _vb << ")\n";                                                         \
            std::exit(1);                                                                                \
        }                                                                                                \
    } while (0)

#define CHECK_OK(expr)                                                         \
    do {                                                                       \
        auto&& _r = (expr);                                                    \
        if (!_r.ok()) {                                                        \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " — " #expr \
                      << " error: " << ::wavedb::test::ErrMsg(_r) << "\n";     \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

#define RUN_TESTS()                                  \
    int main() {                                     \
        std::cout << "=== " << __FILE__ << " ===\n"; \
        run_tests();                                 \
        std::cout << "PASS\n";                       \
        return 0;                                    \
    }

// 辅助：创建临时目录
inline std::string MakeTempDir(const char* prefix) {
    std::string path = std::string("/tmp/") + prefix + "_XXXXXX";
    char* buf = &path[0];
    if (::mkdtemp(buf) == nullptr) {
        std::cerr << "mkdtemp failed\n";
        std::exit(1);
    }
    return path;
}

inline void RemoveDir(const std::string& path) { std::system(("rm -rf " + path).c_str()); }
