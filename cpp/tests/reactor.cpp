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

#include <koru/reactor.hpp>

#include <cerrno>
#include <coroutine>
#include <cstring>
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
    Completion c = co_await r.submit(sqe::read(0, handle, slot.index(), 0, len), std::move(slot));
    out->resumed++;
    out->res = c.res;
    // The slot comes back with the result and goes to the pool here.
}

Frame checksum_slot(Reactor &r, BufSlot slot, uint32_t len, Outcome *out)
{
    Completion c = co_await r.submit(sqe::checksum(0, slot.index(), 0, len), std::move(slot));
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
        Completion c =
            co_await rr.submit(sqe::open(0, p.index(), 0, len, KORU_O_RDONLY), std::move(p));
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
