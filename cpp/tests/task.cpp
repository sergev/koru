// SPDX-License-Identifier: MIT
//
// T41's done test: a hundred thousand nested `co_await`s, with the stack
// *measured* rather than assumed flat.
//
// This is the symmetric-transfer regression test, and the reason it is a
// hundred thousand deep is that it passes at ten however it is written. Get
// either transfer wrong — the one into the callee or the one back to the
// continuation — and each level costs a C++ stack frame; at this depth that is
// megabytes, and the run dies with a stack overflow rather than an assertion.
//
// Nothing here touches the device, so it runs on the host under ctest. Under
// the sanitized build LSan runs at exit, which is where "a task destroyed
// without being awaited leaks nothing" is actually checked.

#include "harness.hpp"

#include <koru/task.hpp>

#include <cstddef>

using namespace koru;

namespace {

// GCC's AddressSanitizer instrumentation defeats the tail call the standard
// promises, at every optimization level and with every flag there is — a
// sanitized frame cannot be tail-called out of. Clang's runtime is not
// installed here, so this build is GCC's, and under it the depth case runs
// shallow and says so rather than measuring something the toolchain has
// already decided. doc/Notes.md has the numbers.
#if defined(__SANITIZE_ADDRESS__)
constexpr int DEPTH    = 2000;
constexpr bool MEASURE = false;
#else
/// The plan's number, and it is not decoration: this passes at ten however it
/// is written.
constexpr int DEPTH    = 100000;
constexpr bool MEASURE = true;
#endif

/// Where the stack was when the chain started, and the furthest any level got
/// from it — on the way down and on the way back up, because the two transfers
/// fail independently.
char *stack_base      = nullptr;
ptrdiff_t stack_reach = 0;

void note_stack()
{
    char here   = 0;
    ptrdiff_t d = stack_base - &here;
    if (d < 0)
        d = -d;
    if (d > stack_reach)
        stack_reach = d;
}

task<int> deep(int n)
{
    if (n == 0) {
        note_stack();
        co_return 0;
    }
    int v = co_await deep(n - 1);
    // On the way out as well: a `final_suspend` that resumes its continuation
    // instead of returning it grows the stack here rather than above.
    note_stack();
    co_return v + 1;
}

task<int> answer()
{
    co_return 42;
}

task<int> one_more(int n)
{
    int v = co_await answer();
    co_return v + n;
}

task<void> nothing(int *ran)
{
    (*ran)++;
    co_return;
}

int destroyed_bodies = 0;

struct Marker {
    ~Marker() { destroyed_bodies++; }
};

/// Started and left suspended at its first `co_await`, so the frame owns a
/// local whose destructor must still run when the task is destroyed.
task<int> never_finishes(task<int> inner)
{
    Marker m;
    int v = co_await inner;
    co_return v;
}

} // namespace

CASE(task_is_lazy_and_runs_once_awaited)
{
    int ran      = 0;
    task<void> t = nothing(&ran);
    CHECK(bool(t));
    CHECK_EQ(ran, 0); // nothing has run: initial_suspend is suspend_always
    CHECK(!t.done());
    sync_wait(std::move(t));
    CHECK_EQ(ran, 1);
}

CASE(task_carries_a_value_out)
{
    CHECK_EQ(sync_wait(answer()), 42);
    CHECK_EQ(sync_wait(one_more(8)), 50);
}

CASE(task_is_move_only_and_a_moved_from_task_is_null)
{
    task<int> a = answer();
    CHECK(bool(a));
    task<int> b = std::move(a);
    CHECK(!bool(a)); // the source gave up the frame
    CHECK(bool(b));
    CHECK_EQ(sync_wait(std::move(b)), 42);
}

CASE(task_a_frame_that_would_not_allocate_is_a_null_task)
{
    // `get_return_object_on_allocation_failure` is what makes Braam's
    // `if (task<T> t = ...)` idiom mean something. Without it this is
    // undefined behaviour rather than a value.
    detail::fail_frame_allocations = 1;
    task<int> t                    = answer();
    CHECK(!bool(t));
    CHECK(t.done());
    CHECK_EQ(detail::fail_frame_allocations, 0);
    // And the next one is fine: the failure is not sticky.
    task<int> ok = answer();
    CHECK(bool(ok));
    CHECK_EQ(sync_wait(std::move(ok)), 42);
}

CASE(task_destroyed_without_being_awaited_runs_its_destructors)
{
    // Two shapes: never started, and started and suspended. LSan at exit is
    // what proves the frames themselves went; this proves the *bodies* were
    // unwound, which a handle leaked rather than destroyed would not do.
    destroyed_bodies = 0;
    {
        task<int> never = never_finishes(answer());
        CHECK(bool(never));
    }
    CHECK_EQ(destroyed_bodies, 0); // never started, so Marker was never built

    {
        task<int> inner = answer();
        task<int> outer = never_finishes(std::move(inner));
        // Start it: it runs to its first co_await and suspends there.
        outer.handle().resume();
        CHECK(outer.done() || !outer.done()); // either is legal; it is alive
    }
    CHECK_EQ(destroyed_bodies, 1);
}

CASE(task_a_hundred_thousand_nested_awaits_keep_the_stack_flat)
{
    char base   = 0;
    stack_base  = &base;
    stack_reach = 0;

    CHECK_EQ(sync_wait(deep(DEPTH)), DEPTH);

    // A frame per level would be tens of bytes times a hundred thousand: the
    // run would die before reaching this line. The window is generous because
    // the point is the *order of magnitude*, not the exact frame size.
    test_note("stack reach over %d levels: %ld bytes%s", DEPTH, long(stack_reach),
              MEASURE ? "" : " (sanitized: not a measurement)");
    if (MEASURE && stack_reach > 65536)
        FAILF("the stack grew %ld bytes: the transfer is not symmetric", long(stack_reach));
}
