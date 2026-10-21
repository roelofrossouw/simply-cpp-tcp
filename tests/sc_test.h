#pragma once
// Minimal assertion helpers for the simply-cpp ctest suite.
//
// Unlike assert() these stay active in Release builds, keep running after the
// first failure so a single ctest run reports everything that is broken, and
// exit non-zero only via TEST_SUMMARY() at the end of main().

#include <cmath>
#include <concepts>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace sc_test {
    inline int checks_run = 0;
    inline int checks_failed = 0;
    inline std::string current_section{};

    template<typename T>
    concept streamable = requires(std::ostream &os, const T &value) { os << value; };

    template<typename T>
    std::string show(const T &value) {
        if constexpr (std::same_as<std::decay_t<T>, bool>) {
            return value ? "true" : "false";
        } else if constexpr (streamable<T>) {
            std::ostringstream oss;
            oss.precision(17);
            oss << value;
            return oss.str();
        } else {
            return "<not printable>";
        }
    }

    inline void report(const bool passed, const std::string_view expression, const char *file, const int line,
                       const std::string &detail = {}) {
        ++checks_run;
        if (passed) return;
        ++checks_failed;
        std::cerr << file << ':' << line << ": FAILED";
        if (!current_section.empty()) std::cerr << " [" << current_section << ']';
        std::cerr << ": " << expression;
        if (!detail.empty()) std::cerr << "\n    " << detail;
        std::cerr << std::endl;
    }

    template<typename A, typename B>
    void report_binary(const bool passed, const std::string_view expression, const A &lhs, const B &rhs,
                       const char *file, const int line) {
        report(passed, expression, file, line, passed ? std::string{} : "lhs = " + show(lhs) + "\n    rhs = " + show(rhs));
    }

    inline void section(const std::string &name) {
        current_section = name;
        std::cout << "-- " << name << std::endl;
    }

    inline int summary() {
        std::cout << (checks_failed == 0 ? "PASSED" : "FAILED") << ": " << (checks_run - checks_failed) << '/'
                << checks_run << " checks passed." << std::endl;
        return checks_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
} // namespace sc_test

#define SECTION(name) ::sc_test::section(name)
#define TEST_SUMMARY() return ::sc_test::summary()

#define CHECK(expr) ::sc_test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#define CHECK_MSG(expr, message) ::sc_test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__, (message))

#define CHECK_EQ(lhs, rhs)                                                                                             \
    do {                                                                                                               \
        const auto &sc_test_lhs = (lhs);                                                                               \
        const auto &sc_test_rhs = (rhs);                                                                               \
        ::sc_test::report_binary(sc_test_lhs == sc_test_rhs, #lhs " == " #rhs, sc_test_lhs, sc_test_rhs, __FILE__,     \
                                 __LINE__);                                                                            \
    } while (false)

#define CHECK_NE(lhs, rhs)                                                                                             \
    do {                                                                                                               \
        const auto &sc_test_lhs = (lhs);                                                                               \
        const auto &sc_test_rhs = (rhs);                                                                               \
        ::sc_test::report_binary(sc_test_lhs != sc_test_rhs, #lhs " != " #rhs, sc_test_lhs, sc_test_rhs, __FILE__,     \
                                 __LINE__);                                                                            \
    } while (false)

#define CHECK_LT(lhs, rhs)                                                                                             \
    do {                                                                                                               \
        const auto &sc_test_lhs = (lhs);                                                                               \
        const auto &sc_test_rhs = (rhs);                                                                               \
        ::sc_test::report_binary(sc_test_lhs < sc_test_rhs, #lhs " < " #rhs, sc_test_lhs, sc_test_rhs, __FILE__,       \
                                 __LINE__);                                                                            \
    } while (false)

// Inclusive range check, handy for timings and other non-deterministic values.
#define CHECK_BETWEEN(value, low, high)                                                                                \
    do {                                                                                                               \
        const auto &sc_test_value = (value);                                                                           \
        ::sc_test::report(sc_test_value >= (low) && sc_test_value <= (high),                                           \
                          #low " <= " #value " <= " #high, __FILE__, __LINE__,                                         \
                          "value = " + ::sc_test::show(sc_test_value));                                                \
    } while (false)

#define CHECK_NEAR(lhs, rhs, tolerance)                                                                                \
    do {                                                                                                               \
        const double sc_test_lhs = static_cast<double>(lhs);                                                           \
        const double sc_test_rhs = static_cast<double>(rhs);                                                           \
        const double sc_test_tol = static_cast<double>(tolerance);                                                     \
        ::sc_test::report(std::fabs(sc_test_lhs - sc_test_rhs) <= sc_test_tol, #lhs " ~= " #rhs, __FILE__, __LINE__,   \
                          "lhs = " + ::sc_test::show(sc_test_lhs) + ", rhs = " + ::sc_test::show(sc_test_rhs) +        \
                              ", tolerance = " + ::sc_test::show(sc_test_tol));                                        \
    } while (false)

#define CHECK_THROWS_AS(expr, exception_type)                                                                          \
    do {                                                                                                               \
        bool sc_test_threw = false;                                                                                    \
        try {                                                                                                          \
            (void) (expr);                                                                                             \
        } catch (const exception_type &) {                                                                             \
            sc_test_threw = true;                                                                                      \
        } catch (...) {                                                                                                \
            ::sc_test::report(false, #expr " throws " #exception_type, __FILE__, __LINE__,                             \
                              "threw a different exception type");                                                     \
            break;                                                                                                     \
        }                                                                                                              \
        ::sc_test::report(sc_test_threw, #expr " throws " #exception_type, __FILE__, __LINE__, "nothing was thrown");  \
    } while (false)

#define CHECK_NOTHROW(expr)                                                                                            \
    do {                                                                                                               \
        try {                                                                                                          \
            (void) (expr);                                                                                             \
            ::sc_test::report(true, #expr " does not throw", __FILE__, __LINE__);                                      \
        } catch (const std::exception &sc_test_error) {                                                                \
            ::sc_test::report(false, #expr " does not throw", __FILE__, __LINE__, sc_test_error.what());               \
        } catch (...) {                                                                                                \
            ::sc_test::report(false, #expr " does not throw", __FILE__, __LINE__, "unknown exception");                \
        }                                                                                                              \
    } while (false)
