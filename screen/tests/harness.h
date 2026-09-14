// SPDX-License-Identifier: MIT
//
// Braam's test/unit/harness.h, minus the coroutine half: assertions that count
// their failures, and the one terminal every case uses.
#pragma once

#include "screen.h"
#include "text.h"

// The terminal every case that touches a screen uses. One, as Braam's suite
// has one, because a second needs a client and that is the daemon's test.
Term &t0();

void test_begin(Str name);
void test_check(bool ok, Str expr, Str file, u32 line);
void test_check_eq(u64 a, u64 b, Str expr, Str file, u32 line);
u32 test_failures();

#define CHECK(expr)    test_check((expr), #expr, __FILE_NAME__, __LINE__)
#define CHECK_EQ(a, b) test_check_eq(u64(a), u64(b), #a " == " #b, __FILE_NAME__, __LINE__)

void test_screen();
void test_ansi();
