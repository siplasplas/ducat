#pragma once

// Minimal test helpers. A test program calls CHECK and CHECK_EQ, then returns
// check::result() from main, so ctest sees a failure as a non-zero exit code.

#include <iostream>

namespace check {

inline int failures = 0;

inline void fail(const char* file, int line, const char* expression)
{
    ++failures;
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
}

template <typename A, typename B>
void fail_eq(const char* file, int line, const char* expression, const A& a, const B& b)
{
    fail(file, line, expression);
    std::cerr << "    left:  " << a << "\n    right: " << b << '\n';
}

inline int result()
{
    if (failures != 0)
        std::cerr << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}

} // namespace check

#define CHECK(condition)                                     \
    do {                                                     \
        if (!(condition))                                    \
            ::check::fail(__FILE__, __LINE__, #condition);   \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        const auto& check_a_ = (a);                                        \
        const auto& check_b_ = (b);                                        \
        if (!(check_a_ == check_b_))                                       \
            ::check::fail_eq(__FILE__, __LINE__, #a " == " #b, check_a_, check_b_); \
    } while (0)
