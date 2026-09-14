// SPDX-License-Identifier: MIT
//
// T43's done test: the C++ mirror of T16.
//
// A whole-slot `READ` races a timer; whichever loses has its **frame destroyed
// while its op may still be in flight**, thousands of times, under ASan and
// UBSan. What must hold is what T16 established on the Rust side:
//
//   - a live op's entry keeps its slot until the CQE lands, so the slot is not
//     back in the pool one moment earlier;
//   - the op that reuses that slot index afterwards is not refused `-EBUSY`,
//     which is the assertion the whole task is named for;
//   - slab entries do not accumulate, and at the end every SQE has exactly one
//     CQE — C1 end to end.
//
// The race is what makes both drop paths reachable: a read that is still live
// takes the `CANCEL` path, and one that finished first takes the cheap one. A
// run that only ever reached one of them would prove half of this.
//
//   KORU_ITERS=100000 raises the count; KORU_SEED replays a run.

#include "common.hpp"

#include <koru/exec.hpp>
#include <koru/reactor.hpp>
#include <koru/task.hpp>

#include <cerrno>
#include <coroutine>
#include <cstdlib>
#include <cstring>
#include <utility>

using namespace koru;

namespace {

/// The same shape the fuzzers use: a PRNG that needs nothing.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint32_t next()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return uint32_t(state >> 33);
    }
    uint32_t below(uint32_t n) { return next() % n; }
};

uint64_t env_u64(const char *name, uint64_t fallback)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return fallback;
    return strtoull(v, nullptr, 10);
}

/// A task holding one op. Destroying it before it finishes is the whole point:
/// the awaiter lives in this frame.
task<int64_t> one_read(Reactor &r, uint32_t handle, BufSlot slot, uint32_t len, int *done)
{
    uint32_t index = slot.index();
    Completion c   = co_await r.submit(sqe::read(0, handle, index, 0, len), std::move(slot));
    (*done)++;
    co_return c.res;
}

} // namespace

CASE(drop_safety_under_a_race_holds_a_slot_until_its_completion)
{
    uint64_t iters = env_u64("KORU_ITERS", 4000);
    uint64_t seed  = env_u64("KORU_SEED", 20260914);
    arm_alarm(unsigned(120 + iters / 100));
    ensure_pattern_file();
    test_note("KORU_ITERS=%llu KORU_SEED=%llu", (unsigned long long)iters,
              (unsigned long long)seed);

    Mapped m = Mapped::shared();
    Reactor r(std::move(m.ring), std::move(m.arena));

    // The handle, through the reactor like everything else.
    int64_t h = 0;
    {
        BufSlot path = r.pool().acquire();
        REQUIRE(path.held());
        std::span<uint8_t> b = path.bytes();
        memset(b.data(), 0, b.size());
        memcpy(b.data(), PATFILE, strlen(PATFILE));
        uint32_t plen   = uint32_t(strlen(PATFILE));
        int done        = 0;
        task<int64_t> t = [](Reactor &rr, BufSlot p, uint32_t len) -> task<int64_t> {
            uint32_t at = p.index();
            Completion c =
                co_await rr.submit(sqe::open(0, at, 0, len, KORU_O_RDONLY), std::move(p));
            co_return c.res;
        }(r, std::move(path), plen);
        t.handle().resume();
        while (!t.done())
            r.pump(1, 0);
        h = t.await_resume();
        (void)done;
    }
    REQUIRE(h > 0);

    size_t baseline = r.pool().free_count();
    Rng rng(seed);
    uint64_t read_won = 0, cancelled = 0, uncancelled = 0;

    for (uint64_t round = 0; round < iters; round++) {
        BufSlot slot = r.pool().acquire();
        if (!slot.held()) {
            FAILF("round %llu: the pool ran out", (unsigned long long)round);
            return;
        }
        uint32_t index          = slot.index();
        uint64_t cancels_before = r.cancels_submitted();
        int finished            = 0;

        // The timer first, so its work item queues ahead of the read's. That
        // ordering is the whole of the race: with the read submitted first it
        // always wins, and the in-flight drop path is never reached.
        uint64_t ns         = rng.below(4) == 0 ? 1000000 : 0;
        int timer_done      = 0;
        task<int64_t> timer = [](Reactor &rr, uint64_t d, int *c) -> task<int64_t> {
            Completion x = co_await rr.submit(sqe::delay_ns(0, d));
            (*c)++;
            co_return x.res;
        }(r, ns, &timer_done);
        timer.handle().resume();

        // The read, in a frame of its own, submitted but not awaited here.
        task<int64_t> reader = one_read(r, uint32_t(h), std::move(slot), SLOT, &finished);
        reader.handle().resume();

        // Whichever lands first wins; pump until one of them is done.
        while (!timer_done && !finished)
            r.pump(1, 0);

        if (!finished) {
            // The read lost: destroy its frame with the op still in flight.
            reader           = {};
            uint64_t cancels = r.cancels_submitted() - cancels_before;
            if (cancels == 1) {
                // Live: the entry keeps the slot until the CQE lands.
                if (r.pool().free_count() != baseline - 1) {
                    FAILF("round %llu: the slot came back before the read's completion",
                          (unsigned long long)round);
                    return;
                }
                int spins = 0;
                while (r.pool().free_count() < baseline) {
                    r.pump(1, 100 * 1000000ull);
                    if (++spins > 1000) {
                        FAILF("round %llu: the cancelled read's slot never came back",
                              (unsigned long long)round);
                        return;
                    }
                }
                cancelled++;
            } else if (cancels == 0) {
                // It completed between the pump and the destroy: a CANCEL
                // would have answered -ENOENT, so none went out.
                uncancelled++;
            } else {
                FAILF("round %llu: one drop, %llu CANCELs", (unsigned long long)round,
                      (unsigned long long)cancels);
                return;
            }

            // The assertion this case is named for. The free list is LIFO, so
            // the slot that comes back is the one the kernel just released.
            BufSlot again = r.pool().acquire();
            if (again.index() != index) {
                FAILF(
                    "round %llu: the free list is not LIFO: got slot %u, want %u "
                    "(cancels %llu, live %zu, inflight %zu)",
                    (unsigned long long)round, again.index(), index, (unsigned long long)cancels,
                    r.live(), r.inflight());
                return;
            }
            int probe_done      = 0;
            task<int64_t> probe = [](Reactor &rr, BufSlot s, int *c) -> task<int64_t> {
                uint32_t at  = s.index();
                Completion x = co_await rr.submit(sqe::checksum(0, at, 0, 4096), std::move(s));
                (*c)++;
                co_return x.res;
            }(r, std::move(again), &probe_done);
            probe.handle().resume();
            while (!probe_done)
                r.pump(1, 0);
            int64_t res = probe.await_resume();
            if (res == -EBUSY) {
                FAILF("round %llu: slot %u was freed while the kernel still held its claim",
                      (unsigned long long)round, index);
                return;
            }
            if (res < 0) {
                FAILF("round %llu: an op on the reused slot failed: %lld",
                      (unsigned long long)round, (long long)res);
                return;
            }
        } else {
            // The read won. Its result is the whole file; the timer is the one
            // dropped, and it holds no slot to probe.
            int64_t res = reader.await_resume();
            if (res != int64_t(PATSIZE)) {
                FAILF("round %llu: the read answered %lld", (unsigned long long)round,
                      (long long)res);
                return;
            }
            reader = {};
            read_won++;
        }
        timer = {};

        // An entry never discarded would grow this without bound.
        if (r.live() > 64) {
            FAILF("round %llu: %zu slab entries are accumulating", (unsigned long long)round,
                  r.live());
            return;
        }

        // Let whatever is still owed land before the next round, so the
        // counters at the end mean what they say.
        for (int i = 0; i < 1000 && (r.inflight() || r.queued()); i++)
            r.pump(1, 200 * 1000000ull);
    }
    disarm_alarm();

    test_note("read won %llu, cancelled %llu, already done %llu", (unsigned long long)read_won,
              (unsigned long long)cancelled, (unsigned long long)uncancelled);

    // One arm reached proves nothing about the other.
    if (read_won < iters / 2)
        FAILF("the read won only %llu of %llu", (unsigned long long)read_won,
              (unsigned long long)iters);
    if (cancelled < iters / 100)
        FAILF("only %llu of %llu drops caught the read in flight", (unsigned long long)cancelled,
              (unsigned long long)iters);

    // C1 end to end, beside what ASan says: an op whose completion never
    // arrived breaks the equality.
    CHECK_EQ(r.inflight(), 0);
    CHECK_EQ(r.live(), 0); // a slab entry outlived its completion otherwise
    CHECK_EQ(r.cqes_reaped(), r.sqes_submitted());
    CHECK_EQ(r.pool().free_count(), SLOTS);
}
