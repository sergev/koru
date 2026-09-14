// SPDX-License-Identifier: MIT
//
// T40's done test: destroy a coroutine frame with an op in flight.
//
// This is the C++ spelling of T16, and the hazard it guards is the one this
// binding is most likely to have. The awaiter lives *inside* the frame; if the
// frame goes while the kernel is still working, the reactor is left holding a
// handle into freed memory and the arena slot the op is writing into.
//
// Three things must hold, and the cases below assert each:
//
//   - the destroyed frame is never resumed — under ASan a resume of a freed
//     handle is a use-after-free, not a mystery;
//   - the slot is not recycled until the CQE lands, because until then the
//     kernel owns it;
//   - an op that reuses that slot afterwards is not refused with -EBUSY.
//
// The coroutine type here is deliberately minimal: `task<T>` is T41, and a
// done test that needed it would be testing two things at once.

#include "common.hpp"

#include <koru/exec.hpp>
#include <koru/reactor.hpp>
#include <koru/task.hpp>

#include <cerrno>
#include <coroutine>
#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

using namespace koru;

namespace {

/// An eager, hand-destroyed coroutine. `initial_suspend` is `suspend_never`,
/// so calling one runs it to its first `co_await`; `final_suspend` is
/// `suspend_always`, so the frame outlives the body and the test decides when
/// it goes.
struct Frame {
    struct promise_type {
        Frame get_return_object()
        {
            return Frame{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> h;

    bool done() const { return !h || h.done(); }
    void destroy()
    {
        if (h) {
            h.destroy();
            h = {};
        }
    }
};

/// What one op's frame recorded before it ended.
struct Outcome {
    int resumed = 0;
    int64_t res = 0;
};

Frame read_into_slot(Reactor &r, BufSlot slot, uint32_t handle, uint32_t len, Outcome *out)
{
    uint32_t at = slot.index();
    Completion c = co_await r.submit(sqe::read(0, handle, at, 0, len), std::move(slot));
    out->resumed++;
    out->res = c.res;
    // The slot comes back with the result and goes to the pool here.
}

Frame checksum_slot(Reactor &r, BufSlot slot, uint32_t len, Outcome *out)
{
    uint32_t at = slot.index();
    Completion c = co_await r.submit(sqe::checksum(0, at, 0, len), std::move(slot));
    out->resumed++;
    out->res = c.res;
}

Reactor make_reactor()
{
    Mapped m = Mapped::shared();
    return Reactor(std::move(m.ring), std::move(m.arena));
}

/// Pump until nothing is owed, or give up. False means something is stuck,
/// which is a finding rather than a flake.
bool drain(Reactor &r, int rounds = 200)
{
    for (int i = 0; i < rounds; i++) {
        if (r.inflight() == 0 && r.queued() == 0)
            return true;
        r.pump(r.inflight() ? 1 : 0, 200 * 1000 * 1000);
    }
    return r.inflight() == 0 && r.queued() == 0;
}

} // namespace

CASE(reactor_an_op_completes_and_gives_its_slot_back)
{
    // The ordinary path first: without it a drop test proves nothing, because
    // a binding that never completes anything also never resumes a dead frame.
    Reactor r          = make_reactor();
    size_t free_before = r.pool().free_count();
    BufSlot slot       = r.pool().acquire();
    REQUIRE(slot.held());
    for (size_t i = 0; i < 4096; i++)
        slot[i] = 0xa5;
    int64_t want = fnv1a(slot.bytes().subspan(0, 4096));

    Outcome out;
    Frame f = checksum_slot(r, std::move(slot), 4096, &out);
    CHECK_EQ(r.queued(), 1); // queued, and the kernel has not seen it
    CHECK_EQ(out.resumed, 0);

    CHECK(drain(r));
    CHECK_EQ(out.resumed, 1);
    CHECK_EQ(out.res, want);
    CHECK(f.done());
    f.destroy();
    CHECK_EQ(r.pool().free_count(), free_before); // the slot came back
    CHECK_EQ(r.live(), 0);                        // and the slab entry with it
}

CASE(reactor_one_enter_carries_the_batch_and_the_reap)
{
    // Measured, never assumed. A reactor that submits in one ioctl and reaps
    // in another produces the right bytes and passes every behavioural case
    // here, at twice the syscalls — which is the bug the Rust executor had.
    Reactor r = make_reactor();
    Outcome a, b;
    BufSlot s1 = r.pool().acquire();
    BufSlot s2 = r.pool().acquire();
    REQUIRE(s1.held() && s2.held());
    Frame f1 = checksum_slot(r, std::move(s1), 4096, &a);
    Frame f2 = checksum_slot(r, std::move(s2), 4096, &b);

    uint64_t before = r.enters();
    CHECK_EQ(r.pump(2, 0), 2); // both, in one ENTER
    CHECK_EQ(r.enters() - before, 1);
    CHECK_EQ(a.resumed, 1);
    CHECK_EQ(b.resumed, 1);
    f1.destroy();
    f2.destroy();
}

CASE(reactor_a_frame_destroyed_before_the_submit_drops_its_sqe)
{
    // Queued, not live: the kernel never sees the SQE at all, so there is
    // nothing to cancel and the slot is free at once.
    Reactor r          = make_reactor();
    size_t free_before = r.pool().free_count();
    BufSlot slot       = r.pool().acquire();
    REQUIRE(slot.held());
    Outcome out;
    Frame f = checksum_slot(r, std::move(slot), 4096, &out);
    CHECK_EQ(r.queued(), 1);

    f.destroy();
    CHECK_EQ(r.live(), 0);                        // the entry went with it
    CHECK_EQ(r.pool().free_count(), free_before); // and so did the slot

    uint64_t before = r.enters();
    CHECK_EQ(r.pump(0, 0), 0); // nothing arrives
    CHECK_EQ(r.enters() - before, 1);
    CHECK_EQ(r.inflight(), 0); // because nothing was ever submitted
    CHECK_EQ(out.resumed, 0);
    CHECK(drain(r));
}

CASE(reactor_a_frame_destroyed_mid_flight_is_never_resumed)
{
    ensure_pattern_file();
    Reactor r = make_reactor();
    // The OPEN goes through the reactor too, so the whole case is one ring.
    int64_t h    = 0;
    BufSlot path = r.pool().acquire();
    REQUIRE(path.held());
    uint32_t plen = 0;
    {
        std::span<uint8_t> b = path.bytes();
        memset(b.data(), 0, b.size());
        plen = uint32_t(strlen(PATFILE));
        memcpy(b.data(), PATFILE, plen);
    }
    Outcome o_open;
    Frame fo = [](Reactor &rr, BufSlot p, uint32_t len, Outcome *out) -> Frame {
        uint32_t at = p.index();
        Completion c =
            co_await rr.submit(sqe::open(0, at, 0, len, KORU_O_RDONLY), std::move(p));
        out->resumed++;
        out->res = c.res;
    }(r, std::move(path), plen, &o_open);
    CHECK(drain(r));
    CHECK_EQ(o_open.resumed, 1);
    h = o_open.res;
    fo.destroy();
    REQUIRE(h > 0);

    size_t free_before = r.pool().free_count();
    BufSlot slot       = r.pool().acquire();
    REQUIRE(slot.held());
    uint32_t index = slot.index();

    Outcome out;
    // A whole 64 KB READ, deferred to a kworker: the window is the common case
    // rather than a race this has to win.
    Frame f = read_into_slot(r, std::move(slot), uint32_t(h), SLOT, &out);
    CHECK_EQ(r.pump(0, 0), 0); // submitted, not yet complete
    CHECK_EQ(r.inflight(), 1);

    f.destroy(); // the hazard: the awaiter's frame goes with the op in flight
    CHECK_EQ(out.resumed, 0);
    // The slot is the kernel's until the CQE lands, so it must not be back in
    // the pool yet whatever the frame did.
    CHECK_EQ(r.pool().free_count(), free_before - 1);
    CHECK(r.live() >= 1); // the entry and its CANCEL probe

    CHECK(drain(r));
    // Now, and only now.
    CHECK_EQ(out.resumed, 0);
    CHECK_EQ(r.pool().free_count(), free_before);
    CHECK_EQ(r.live(), 0);

    // The same index again: the kernel released its busy bit in the same
    // critical section that posted the CQE, so this is not -EBUSY.
    BufSlot again = r.pool().acquire();
    CHECK_EQ(again.index(), index);
    Outcome after;
    Frame f2 = checksum_slot(r, std::move(again), SLOT, &after);
    CHECK(drain(r));
    CHECK_EQ(after.resumed, 1);
    if (after.res < 0)
        FAILF("the reused slot answered %lld", (long long)after.res);
    f2.destroy();

    CHECK_EQ(r.pump(0, 0), 0);
    CHECK_EQ(r.live(), 0);
}

CASE(reactor_two_hundred_frames_destroyed_mid_flight)
{
    // The loop is what makes the use-after-free reachable. Some rounds have
    // the CQE already landed by the time the frame goes and some do not; both
    // are the point, and only the second kind is the hazard. Under ASan a
    // resumed dangling handle is a report rather than a mystery.
    arm_alarm(120);
    Reactor r          = make_reactor();
    size_t free_before = r.pool().free_count();
    int mid_flight     = 0;

    for (int round = 0; round < 200; round++) {
        BufSlot slot = r.pool().acquire();
        if (!slot.held()) {
            FAILF("round %d: the pool ran out, so a slot leaked", round);
            return;
        }
        Outcome out;
        Frame f = checksum_slot(r, std::move(slot), SLOT, &out);
        r.pump(0, 0); // submit, and reap whatever is already there

        // A frame that finished inside that pump was resumed, and rightly. It
        // is the count *after* the destroy that must not move.
        int was = out.resumed;
        if (was == 0)
            mid_flight++;
        f.destroy();

        if (!drain(r)) {
            FAILF("round %d: something was left owed", round);
            return;
        }
        if (out.resumed != was) {
            FAILF("round %d: a destroyed frame was resumed", round);
            return;
        }
        if (r.pool().free_count() != free_before) {
            FAILF("round %d: %zu slots free, want %zu", round, r.pool().free_count(), free_before);
            return;
        }
    }
    disarm_alarm();
    CHECK_EQ(r.live(), 0);
    // A run where every frame had already finished would test the destructor
    // and nothing else, which is the trap the Rust side records.
    test_note("destroyed while in flight: %d of 200", mid_flight);
    if (mid_flight < 50)
        FAILF("only %d of 200 rounds reached the window", mid_flight);
}

// ---------------------------------------------------------------------------
// T42 - the executor
// ---------------------------------------------------------------------------

namespace {

/// An awaiter that never completes and that nothing can complete: the shape a
/// program has when it waits for something that does not exist.
struct never {
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

task<void> wait_for_nothing()
{
    co_await never{};
}

task<int> count_timers(Executor &ex, int n, int *done)
{
    for (int i = 0; i < n; i++) {
        // Armed longest first, so submission order and completion order differ.
        uint64_t ms = uint64_t(n - i);
        ex.spawn([](Reactor &r, uint64_t d, int *c) -> task<void> {
            co_await r.submit(sqe::delay_ns(0, d * 1000000));
            (*c)++;
        }(ex.reactor(), ms, done));
    }
    // Nothing of ours is in flight, so this is the executor's own wait: the
    // spawned tasks are what keeps the ring from going quiet.
    while (*done < n)
        co_await ex.reactor().submit(sqe::delay_ns(0, 1000000));
    co_return *done;
}

Executor make_executor()
{
    Mapped m = Mapped::shared();
    return Executor(std::move(m.ring), std::move(m.arena));
}

} // namespace

CASE(exec_spawned_tasks_run_and_are_reaped)
{
    Executor ex = make_executor();
    int done    = 0;
    CHECK_EQ(ex.run(count_timers(ex, 4, &done)), 4);
    CHECK_EQ(done, 4);
    ex.drain();
    CHECK_EQ(ex.spawned(), 0); // every frame the executor owned is gone
    CHECK_EQ(ex.reactor().live(), 0);
}

CASE(exec_a_spawn_starts_the_task_at_once)
{
    // Braam's `proc_spawn` runs it; a spawn that only queued would leave the
    // ring empty until the next turn, and a program that spawns and then waits
    // would deadlock on its own timers.
    Executor ex = make_executor();
    int ran     = 0;
    ex.spawn([](Reactor &r, int *c) -> task<void> {
        (*c)++;
        co_await r.submit(sqe::delay_ns(0, 1000000));
        (*c)++;
    }(ex.reactor(), &ran));
    CHECK_EQ(ran, 1);                   // the body ran up to its first await
    CHECK_EQ(ex.reactor().queued(), 1); // and its op is queued
    ex.drain();
    CHECK_EQ(ran, 2);
    CHECK_EQ(ex.spawned(), 0);
}

CASE(exec_waiting_for_what_cannot_arrive_is_not_a_spin)
{
    // The same foot-gun `ENTER` closes on the kernel side: with nothing in
    // flight, nothing queued and nothing ready, a park returns at once and a
    // loop around it spins for ever. The executor says so and stops.
    arm_alarm(60);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // The child arms its own watchdog. Without it an executor that spins
        // instead of refusing leaves an orphan running for ever, and the whole
        // gate hangs rather than failing — which is what the first version of
        // this case did when the guard was deleted to check it.
        arm_alarm(20);
        Executor ex = make_executor();
        ex.run(wait_for_nothing()); // must not return
        _exit(7);                   // and must not reach this
    }
    int status = 0;
    waitpid(pid, &status, 0);
    disarm_alarm();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 7)
        FAILF("run() returned from a task that cannot complete");
    // abort() from the executor, which is SIGABRT rather than a hang. The
    // watchdog's own exit status, 99, is what a spin looks like from here.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99)
        FAILF("the executor spun instead of refusing");
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGABRT);
}

CASE(exec_destroying_the_executor_takes_its_spawned_frames)
{
    // A spawned task suspended on an op still owns a frame. The executor owns
    // that frame, so its destructor is what frees it — and the op's slab entry
    // and slot outlive both, until the CQE lands.
    size_t free_before = 0;
    {
        Executor ex = make_executor();
        free_before = ex.pool().free_count();
        ex.spawn([](Reactor &r) -> task<void> {
            BufSlot s = r.pool().acquire();
            co_await r.submit(sqe::delay_ns(0, 2000 * 1000000ull), std::move(s));
        }(ex.reactor()));
        CHECK_EQ(ex.spawned(), 1);
        CHECK_EQ(ex.pool().free_count(), free_before - 1);
        // Destroyed here, with a two-second delay still in flight. Under ASan
        // a frame left behind is a leak and a frame resumed is worse.
    }
    CHECK(free_before > 0);
}
