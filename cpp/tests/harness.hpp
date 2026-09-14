// SPDX-License-Identifier: MIT
//
// The C++ suite's harness: named cases, assertions that count their failures,
// and a filter so a section can be run on its own.
//
// A case that can hang arms an alarm, and stdout is line buffered, because a
// hang that has printed nothing is a hang nobody can diagnose.

#ifndef KORU_TESTS_HARNESS_HPP
#define KORU_TESTS_HARNESS_HPP

#include <cstdint>

using CaseFn = void (*)();

void register_case(const char *name, CaseFn fn);

struct CaseReg {
    CaseReg(const char *name, CaseFn fn) { register_case(name, fn); }
};

/// One case. The name is the whole of its identity: the filter matches on it
/// and the log prints it, exactly as `cargo test` does with a test function.
#define CASE(name)                               \
    static void name();                          \
    static CaseReg koru_reg_##name(#name, name); \
    static void name()

bool test_check(bool ok, const char *expr, const char *file, int line);
bool test_check_eq(int64_t got, int64_t want, const char *expr, const char *file, int line);
bool test_check_str(const char *got, const char *want, const char *expr, const char *file,
                    int line);

/// What a case says when it cannot run. A skip is not a pass: the gate script
/// greps for this marker and fails the run.
void test_skip(const char *what, const char *why);

#define CHECK(e)        test_check((e), #e, __FILE__, __LINE__)
#define CHECK_EQ(a, b)  test_check_eq(int64_t(a), int64_t(b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_STR(a, b) test_check_str((a), (b), #a " == " #b, __FILE__, __LINE__)

/// For what cannot sensibly carry on: the failure is recorded and the case
/// stops rather than dereferencing what it just failed to get.
#define REQUIRE(e)     \
    do {               \
        if (!CHECK(e)) \
            return;    \
    } while (0)

/// A message with the same prefix as a failure, for a case that wants to say
/// which row of a matrix it is on.
void test_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// Fails the current case with a formatted message.
void test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define FAILF(...) test_fail(__FILE__, __LINE__, __VA_ARGS__)

/// Arm a watchdog around anything that can block for ever.
void arm_alarm(unsigned secs);
void disarm_alarm();

#endif // KORU_TESTS_HARNESS_HPP
