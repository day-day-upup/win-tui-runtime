#pragma once

// 阶段 1：极简测试框架（header-only，零依赖）
//
// 约束 A4 允许自建断言宏。这里故意不引入 gtest/catch2：
// 保持离线可构建，也避免把「测试框架」变成项目复杂度的一部分。
//
// 用法：
//   TEST(my_suite_case) { CHECK_EQ(a, b); CHECK_TRUE(cond); }
//   int main() { return tui::test::RunAll(); }
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace tui::test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int& Failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        Registry().push_back({name, std::move(fn)});
    }
};

template <typename T>
std::string Str(const T& v) {
    std::ostringstream os;
    if constexpr (std::is_enum_v<T>) {
        os << static_cast<std::underlying_type_t<T>>(v);
    } else if constexpr (std::is_same_v<T, wchar_t>) {
        // char 流不接受 wchar_t（operator<< 被删除）；转成码点数字。
        os << "wchar(" << static_cast<int>(v) << ")";
    } else if constexpr (std::is_same_v<T, std::wstring>) {
        // 窄流不接受 wchar_t 字符串；逐字符转 ASCII（测试里都是 ASCII 名）。
        for (wchar_t c : v) os << (c >= 32 && c < 127 ? static_cast<char>(c) : '?');
    } else if constexpr (std::is_same_v<T, char> || std::is_same_v<T, signed char> ||
                         std::is_same_v<T, unsigned char>) {
        os << static_cast<int>(v);
    } else {
        os << v;
    }
    return os.str();
}

inline void Fail(const char* file, int line, const char* expr,
                 const std::string& detail) {
    ++Failures();
    std::fprintf(stderr, "  FAIL %s:%d  %s", file, line, expr);
    if (!detail.empty()) std::fprintf(stderr, "  [%s]", detail.c_str());
    std::fprintf(stderr, "\n");
}

inline int RunAll() {
    int passed = 0;
    for (auto& t : Registry()) {
        int before = Failures();
        std::printf("[ RUN ] %s\n", t.name);
        try {
            t.fn();
        } catch (const std::exception& e) {
            Fail(t.name, 0, "unexpected exception", e.what());
        } catch (...) {
            Fail(t.name, 0, "unexpected exception", "unknown");
        }
        if (Failures() == before) {
            std::printf("[  OK ] %s\n", t.name);
            ++passed;
        } else {
            std::printf("[FAIL ] %s\n", t.name);
        }
    }
    std::printf("\n%d/%zu passed, %d failure(s)\n", passed, Registry().size(), Failures());
    return Failures() == 0 ? 0 : 1;
}

}  // namespace tui::test

#define CHECK_TRUE(expr)                                                        \
    do {                                                                        \
        if (!(expr)) ::tui::test::Fail(__FILE__, __LINE__, #expr, "expected true"); \
    } while (0)

#define CHECK_FALSE(expr)                                                       \
    do {                                                                        \
        if ((expr)) ::tui::test::Fail(__FILE__, __LINE__, #expr, "expected false"); \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        auto _va = (a);                                                         \
        auto _vb = (b);                                                         \
        if (!(_va == _vb)) {                                                    \
            ::tui::test::Fail(__FILE__, __LINE__, #a " == " #b,                 \
                              ::tui::test::Str(_va) + " vs " + ::tui::test::Str(_vb)); \
        }                                                                       \
    } while (0)

#define TEST(name)                                                              \
    static void name();                                                         \
    static ::tui::test::Registrar name##_reg(#name, &name);                     \
    static void name()
