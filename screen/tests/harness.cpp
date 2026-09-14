// SPDX-License-Identifier: MIT
//
// The harness and the entry point. Braam's prints through host_log and counts
// failures; this prints to stdout and exits non-zero, which is what ctest reads.

#include "harness.h"

#include <cstdio>
#include <cstdlib>

namespace {

u32 failures;
Str current;
Term *term;

} // namespace

Term &t0()
{
    if (!term) {
        term = term_new();
        if (!term) {
            fprintf(stderr, "the terminal would not allocate\n");
            exit(1);
        }
    }
    return *term;
}

void test_begin(Str name)
{
    current = name;
    printf("== %.*s ==\n", int(name.size()), name.data());
}

void test_check(bool ok, Str expr, Str file, u32 line)
{
    if (ok)
        return;
    failures++;
    printf("FAIL %.*s: %.*s:%u: %.*s\n", int(current.size()), current.data(), int(file.size()),
           file.data(), line, int(expr.size()), expr.data());
}

void test_check_eq(u64 a, u64 b, Str expr, Str file, u32 line)
{
    if (a == b)
        return;
    failures++;
    printf("FAIL %.*s: %.*s:%u: %.*s (%llu != %llu)\n", int(current.size()), current.data(),
           int(file.size()), file.data(), line, int(expr.size()), expr.data(),
           (unsigned long long)a, (unsigned long long)b);
}

u32 test_failures()
{
    return failures;
}

int main()
{
    test_screen();
    test_ansi();
#ifdef KS_HAVE_SDL
    test_render();
#endif

    // The suite owns the terminal, so freeing it here is what lets the leak
    // checker speak for the model rather than for the harness.
    term_free(term);
    term = nullptr;

    u32 bad = test_failures();
    printf("%s: %u failure(s)\n", bad ? "FAIL" : "OK", bad);
    return bad ? 1 : 0;
}
