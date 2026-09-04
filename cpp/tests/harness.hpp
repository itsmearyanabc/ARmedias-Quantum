// A ~60-line test harness.
//
// Phase 0 deliberately has zero external dependencies so that a fresh machine
// with only a compiler and CMake can build and run everything. Catch2 arrives
// via vcpkg in Phase 1, when we start needing its richer matchers and the
// golden-file fixtures.
#pragma once

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace tst {

using Fn = void (*)();

struct Case {
    const char* name;
    Fn          fn;
};

std::vector<Case>& registry();

struct Reg {
    Reg(const char* name, Fn fn) { registry().push_back(Case{name, fn}); }
};

void report_failure(const char* file, int line, const std::string& what);
int  run_all();

template <class A, class B>
void check_eq(const char* file, int line, const char* expr, const A& a, const B& b) {
    if (!(a == b)) {
        std::ostringstream os;
        os << expr << "  (left = " << a << ", right = " << b << ')';
        report_failure(file, line, os.str());
    }
}

inline void check_near(const char* file, int line, const char* expr, double a, double b,
                       double tol) {
    const double diff = std::fabs(a - b);
    if (!(diff <= tol)) {
        std::ostringstream os;
        os.precision(12);
        os << expr << "  (left = " << a << ", right = " << b << ", diff = " << diff
           << ", tol = " << tol << ')';
        report_failure(file, line, os.str());
    }
}

}  // namespace tst

#define XAU_TEST(name)                                  \
    static void name();                                 \
    static ::tst::Reg xau_reg_##name(#name, name);      \
    static void name()

#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) ::tst::report_failure(__FILE__, __LINE__, #expr);  \
    } while (0)

#define REQUIRE(expr)                                                   \
    do {                                                                \
        if (!(expr)) {                                                  \
            ::tst::report_failure(__FILE__, __LINE__, #expr);           \
            return;                                                     \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b) ::tst::check_eq(__FILE__, __LINE__, #a " == " #b, (a), (b))

#define CHECK_NEAR(a, b, tol) \
    ::tst::check_near(__FILE__, __LINE__, #a " ~= " #b, (a), (b), (tol))

#define CHECK_THROWS(expr)                                                        \
    do {                                                                          \
        bool threw = false;                                                       \
        try {                                                                     \
            expr;                                                                 \
        } catch (...) {                                                           \
            threw = true;                                                         \
        }                                                                         \
        if (!threw)                                                               \
            ::tst::report_failure(__FILE__, __LINE__, "expected a throw: " #expr); \
    } while (0)
