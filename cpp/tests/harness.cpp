// SPDX-License-Identifier: MIT

#include "harness.hpp"

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct Case {
    const char *name;
    CaseFn fn;
};

// A function-local static, because a namespace-scope vector would be
// constructed after the first CaseReg runs.
std::vector<Case> &cases()
{
    static std::vector<Case> v;
    return v;
}

unsigned failures_in_case;
unsigned failures_total;
unsigned skips;
const char *current = "";

void alarm_died(int)
{
    // _exit, not exit: a full stdout buffer would take the diagnosis with it.
    static const char msg[] = "\nKORU-CPP-TIMEOUT in case\n";
    ssize_t n               = write(2, msg, sizeof(msg) - 1);
    (void)n;
    _exit(99);
}

} // namespace

void register_case(const char *name, CaseFn fn)
{
    cases().push_back(Case{ name, fn });
}

bool test_check(bool ok, const char *expr, const char *file, int line)
{
    if (!ok) {
        printf("FAIL %s: %s:%d: %s\n", current, file, line, expr);
        failures_in_case++;
    }
    return ok;
}

bool test_check_eq(int64_t got, int64_t want, const char *expr, const char *file, int line)
{
    if (got != want) {
        printf("FAIL %s: %s:%d: %s (%lld != %lld)\n", current, file, line, expr, (long long)got,
               (long long)want);
        failures_in_case++;
        return false;
    }
    return true;
}

bool test_check_str(const char *got, const char *want, const char *expr, const char *file, int line)
{
    bool ok = got && want && strcmp(got, want) == 0;
    if (!ok) {
        printf("FAIL %s: %s:%d: %s ([%s] != [%s])\n", current, file, line, expr,
               got ? got : "(null)", want ? want : "(null)");
        failures_in_case++;
    }
    return ok;
}

void test_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("   ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL %s: %s:%d: ", current, file, line);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures_in_case++;
}

void test_skip(const char *what, const char *why)
{
    printf("KORU-CPP-SKIP %s: %s\n", what, why);
    skips++;
}

void arm_alarm(unsigned secs)
{
    signal(SIGALRM, alarm_died);
    alarm(secs);
}

void disarm_alarm()
{
    alarm(0);
}

int main(int argc, char **argv)
{
    // Line buffered: a case that hangs has already printed its own name.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::vector<std::string> filters;
    for (int i = 1; i < argc; i++)
        filters.push_back(argv[i]);

    unsigned ran = 0, failed = 0;
    for (const Case &c : cases()) {
        if (!filters.empty()) {
            bool want = false;
            for (const std::string &f : filters)
                if (strstr(c.name, f.c_str()))
                    want = true;
            if (!want)
                continue;
        }
        current          = c.name;
        failures_in_case = 0;
        printf("== %s ==\n", c.name);
        // Long enough for the heaviest case, short enough to be a diagnosis.
        arm_alarm(180);
        c.fn();
        disarm_alarm();
        ran++;
        if (failures_in_case) {
            failed++;
            failures_total += failures_in_case;
        }
    }

    printf("\ncases: %u run, %u failed, %u skipped\n", ran, failed, skips);
    printf("OK: %u failure(s)\n", failures_total);
    return failures_total == 0 && skips == 0 ? 0 : 1;
}
