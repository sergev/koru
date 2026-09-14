// SPDX-License-Identifier: MIT
//
// The device suite, C++: T4-T11 plus the T3 matrices, re-expressed against
// libkoru and mirroring rust/sys/tests/kernel.rs case for case.
//
// Needs /dev/koru and root, so it runs in the VM under scripts/run-cpp.sh.
// Cases run one at a time in one process: several fork, and several read
// process-global counters.
//
// The point of this file is not coverage — the Rust suite already has it. It
// is that any divergence between the two is an ABI ambiguity, found before
// coroutines can hide it.

#include "common.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace koru;

namespace {

/// One millisecond, in nanoseconds.
constexpr uint64_t MS = 1000000;

} // namespace

CASE(smoke_nop_completes)
{
    Mapped m = Mapped::shared();
    CHECK_EQ(m.run_one(sqe::nop(0x01)), 0);
    m.assert_quiesced();
}

CASE(smoke_arena_comes_back_zeroed_from_setup)
{
    Mapped m      = Mapped::shared();
    bool all_zero = true;
    for (uint8_t b : m.slot(1))
        all_zero = all_zero && b == 0;
    CHECK(all_zero);
}

CASE(smoke_checksum_matches_the_host)
{
    Mapped m = Mapped::shared();
    m.fill(0, [](size_t) { return uint8_t(0xa5); });
    int64_t want = fnv1a(m.slot(0).subspan(0, 4096));
    CHECK_EQ(m.run_one(sqe::checksum(0x02, 0, 0, 4096)), want);
    m.assert_quiesced();
}

CASE(smoke_the_device_is_there)
{
    // Fails loudly rather than skipping: a suite that passes without the
    // device proves nothing.
    result<Ring> r = Ring::open();
    CHECK(r.ok());
}

// ---------------------------------------------------------------------------
// T4 - ENTER
// ---------------------------------------------------------------------------

namespace {

/// Every opcode plus two that do not exist.
constexpr uint8_t ALL_OPCODES[] = {
    KORU_OP_NOP,
    KORU_OP_DELAY_NS,
    KORU_OP_OPEN,
    KORU_OP_READ,
    KORU_OP_CLOSE,
    KORU_OP_CANCEL,
    KORU_OP_CHECKSUM,
    KORU_OP_WRITE,
    KORU_OP_ADOPT_FD,
    KORU_OP_POLL_ADD,
    KORU_OP_STAT,
    KORU_OP_TRUNCATE,
    KORU_OP_UTIMES,
    KORU_OP_READLINK,
    14,
    200,
};

} // namespace

CASE(enter_returns_the_count_of_sqes_consumed)
{
    Mapped m = Mapped::shared();
    koru_sqe sq[8];
    for (uint64_t i = 0; i < 8; i++)
        sq[i] = sqe::nop(0x1000 + i);
    koru_cqe cq[16] = {};
    EnterResult r   = m.ring.enter(sq, cq, 8);
    REQUIRE(r.ok());

    CHECK_EQ(r.value().consumed, 8);
    CHECK_EQ(r.value().progress.completed, 8);
    CHECK_EQ(r.value().progress.submitted, 8);
    // E1 reports the two independently, so agreement is worth asserting.
    CHECK_EQ(r.value().consumed, r.value().progress.submitted);

    uint64_t i = 0;
    for (const koru_cqe &c : r.value().cqes(cq)) {
        CHECK_EQ(c.user_data, 0x1000 + i);
        CHECK_EQ(c.res, 0);
        CHECK_EQ(c.flags, 0);
        CHECK_EQ(c.rsvd0, 0);
        CHECK_EQ(c.extra, 0);
        i++;
    }
    m.assert_quiesced();
}

CASE(enter_an_unknown_opcode_is_a_cqe_not_an_ioctl_error)
{
    // Rule E1.
    Mapped m      = Mapped::shared();
    koru_sqe s    = {};
    s.opcode      = 200;
    s.user_data   = 0x2000;
    koru_cqe cq   = {};
    EnterResult r = m.ring.run_one(s, cq);
    REQUIRE(r.ok());
    CHECK_EQ(r.value().consumed, 1);
    CHECK_EQ(cq.user_data, 0x2000);
    CHECK_EQ(cq.res, -EINVAL);
    m.assert_quiesced();
}

CASE(enter_a_field_the_opcode_does_not_read_must_be_zero)
{
    Mapped m   = Mapped::shared();
    koru_sqe s = sqe::nop(0x2001);
    s.len      = 1;
    CHECK_EQ(m.run_one(s), -EINVAL);
    m.assert_quiesced();
}

CASE(enter_a_non_zero_sqe_rsvd0_is_einval_for_every_opcode)
{
    Mapped m = Mapped::shared();
    for (uint8_t op : ALL_OPCODES) {
        koru_sqe s  = {};
        s.opcode    = op;
        s.rsvd0     = 1;
        s.user_data = 0x2100;
        if (m.run_one(s) != -EINVAL)
            FAILF("opcode %u", op);
    }
    m.assert_quiesced();
}

CASE(enter_an_unknown_sqe_flag_bit_is_einval_for_every_opcode)
{
    Mapped m = Mapped::shared();
    for (uint8_t op : ALL_OPCODES) {
        koru_sqe s  = {};
        s.opcode    = op;
        s.flags     = 1;
        s.user_data = 0x2200;
        if (m.run_one(s) != -EINVAL)
            FAILF("opcode %u", op);
    }
    m.assert_quiesced();
}

CASE(enter_a_mixed_batch_completes_every_sqe)
{
    // C1: a malformed SQE is still consumed and still produces one CQE.
    Mapped m = Mapped::shared();
    koru_sqe sq[8];
    for (uint64_t i = 0; i < 8; i++) {
        uint64_t ud = 0x3000 + i;
        if (i % 2 == 1) {
            sq[i]           = koru_sqe{};
            sq[i].opcode    = 250;
            sq[i].user_data = ud;
        } else {
            sq[i] = sqe::nop(ud);
        }
    }
    koru_cqe cq[8] = {};
    EnterResult r  = m.ring.enter(sq, cq, 8);
    REQUIRE(r.ok());
    CHECK_EQ(r.value().consumed, 8);
    CHECK_EQ(r.value().progress.completed, 8);

    for (uint64_t i = 0; i < 8; i++) {
        const koru_cqe &c = find_cqe(cq, 0x3000 + i);
        int64_t want      = i % 2 == 1 ? -EINVAL : 0;
        if (c.res != want)
            FAILF("cqe %llu: res %lld, want %lld", (unsigned long long)i, (long long)c.res,
                  (long long)want);
    }
    m.assert_quiesced();
}

CASE(enter_a_short_cq_space_leaves_the_rest_queued)
{
    Mapped m = Mapped::shared();
    koru_sqe sq[8];
    for (uint64_t i = 0; i < 8; i++)
        sq[i] = sqe::nop(0x4000 + i);
    koru_cqe cq[3] = {};
    EnterResult r  = m.ring.enter(sq, cq, 3);
    REQUIRE(r.ok());
    CHECK_EQ(r.value().consumed, 8);           // all 8 are consumed
    CHECK_EQ(r.value().progress.completed, 3); // only 3 fit

    koru_cqe cq2[16] = {};
    EnterResult r2   = m.ring.reap(cq2, 5);
    REQUIRE(r2.ok());
    CHECK_EQ(r2.value().consumed, 0);
    CHECK_EQ(r2.value().progress.completed, 5); // the other 5 arrive later
    uint64_t i = 0;
    for (const koru_cqe &c : r2.value().cqes(cq2)) {
        CHECK_EQ(c.user_data, 0x4003 + i);
        i++;
    }
    m.assert_quiesced();
}

CASE(enter_rejection_matrix)
{
    Mapped m       = Mapped::shared();
    koru_params p  = m.ring.params();
    koru_cqe cq[4] = {};

    // Each needs a field the safe API cannot express, so go through enter_raw.
    koru_enter e = {};

    e       = koru_enter{};
    e.flags = 1;
    expect_enter_errno(m.ring.enter_raw(e), EINVAL, "an unknown ENTER flag");

    e             = koru_enter{};
    e.reserved[1] = 1;
    expect_enter_errno(m.ring.enter_raw(e), EINVAL, "a non-zero reserved word");

    e           = koru_enter{};
    e.to_submit = p.sq_entries + 1;
    expect_enter_errno(m.ring.enter_raw(e), EINVAL, "to_submit past sq_entries");

    e          = koru_enter{};
    e.cq_space = p.cq_entries + 1;
    expect_enter_errno(m.ring.enter_raw(e), EINVAL, "cq_space past cq_entries");

    e              = koru_enter{};
    e.cq_addr      = uint64_t(uintptr_t(cq));
    e.cq_space     = 2;
    e.min_complete = 3;
    expect_enter_errno(m.ring.enter_raw(e), EINVAL, "min_complete past cq_space");

    e           = koru_enter{};
    e.sq_addr   = 0x10;
    e.to_submit = 1;
    expect_enter_errno(m.ring.enter_raw(e), EFAULT, "an unmapped sq_addr");

    m.assert_quiesced();
}

CASE(enter_before_setup_is_einval)
{
    result<Ring> r = Ring::open();
    REQUIRE(r.ok());
    Ring ring    = std::move(r).take();
    koru_enter e = {};
    expect_enter_errno(ring.enter_raw(e), EINVAL, "ENTER on an unconfigured fd");
}

namespace {

/// Submit in batches, never reaping, until the ring stops consuming. Returns
/// the total consumed, which admission control bounds at `cq_entries`.
uint32_t fill_cq(const Mapped &m, uint64_t delay_ns)
{
    uint32_t total = 0;
    for (;;) {
        koru_sqe sq[8];
        for (uint64_t i = 0; i < 8; i++) {
            uint64_t ud = 0x5000 + total + i;
            sq[i]       = delay_ns > 0 ? sqe::delay_ns(ud, delay_ns) : sqe::nop(ud);
        }
        EnterResult r = m.ring.enter(sq, {}, 0);
        if (!r) {
            FAILF("ENTER: %s", r.error().err.message());
            return total;
        }
        total += r.value().consumed;
        if (r.value().consumed < 8)
            return total;
    }
}

koru::SetupConfig small_config()
{
    koru::SetupConfig c;
    c.sq_entries   = 64;
    c.cq_entries   = 128;
    c.slot_size    = 4096;
    c.slot_count   = 8;
    c.handle_count = 8;
    return c;
}

} // namespace

CASE(enter_admission_control_stops_at_a_short_count)
{
    Mapped m      = Mapped::make(small_config());
    uint32_t want = m.ring.params().cq_entries;
    // Submitting past cq_entries stops short.
    CHECK_EQ(fill_cq(m, 0), want);
    CHECK(m.ring.quiesce().ok());
}

CASE(enter_in_flight_work_counts_against_cq_entries)
{
    Mapped m      = Mapped::make(small_config());
    uint32_t want = m.ring.params().cq_entries;
    CHECK_EQ(fill_cq(m, 2 * MS), want);
    CHECK(m.ring.quiesce().ok());
}

// ---------------------------------------------------------------------------
// T5 - DELAY_NS and the blocking wait
// ---------------------------------------------------------------------------

CASE(delay_four_run_concurrently_and_enter_waits)
{
    Mapped m = Mapped::shared();
    koru_sqe sq[4];
    for (uint64_t i = 0; i < 4; i++)
        sq[i] = sqe::delay_ns(0x1000 + i, 50 * MS);
    koru_cqe cq[8] = {};

    uint64_t t0   = now_ms();
    EnterResult r = m.ring.enter(sq, cq, 4);
    uint64_t dt   = now_ms() - t0;
    REQUIRE(r.ok());

    CHECK_EQ(r.value().consumed, 4);
    CHECK_EQ(r.value().progress.submitted, 4);
    CHECK_EQ(r.value().progress.completed, 4);
    if (dt < 40)
        FAILF("ENTER returned after %llums without waiting", (unsigned long long)dt);
    if (dt >= 150)
        FAILF("4 x 50ms took %llums: they ran serially", (unsigned long long)dt);

    for (uint64_t i = 0; i < 4; i++)
        CHECK_EQ(find_cqe(cq, 0x1000 + i).res, 0);
    m.assert_quiesced();
}

CASE(delay_a_short_timeout_reaps_nothing_and_returns_at_once)
{
    Mapped m       = Mapped::shared();
    koru_sqe s     = sqe::delay_ns(0x2000, 2000 * MS);
    koru_cqe cq[4] = {};

    uint64_t t0   = now_ms();
    EnterResult r = m.ring.enter(std::span<const koru_sqe>(&s, 1), cq, 1, 10 * MS);
    uint64_t dt   = now_ms() - t0;
    REQUIRE(r.ok());
    CHECK_EQ(r.value().consumed, 1);
    // A 10ms cap against a 2s delay.
    CHECK_EQ(r.value().progress.completed, 0);
    if (dt >= 500)
        FAILF("waited %llums, not the timeout", (unsigned long long)dt);

    // min_complete 0 returns at once even with work in flight.
    t0 = now_ms();
    r  = m.ring.enter({}, cq, 0);
    REQUIRE(r.ok());
    CHECK_EQ(r.value().consumed, 0);
    CHECK(now_ms() - t0 < 200);

    // And the delay can be cancelled rather than waited out.
    koru_sqe c = sqe::cancel(0x2001, 0x2000);
    r          = m.ring.enter(std::span<const koru_sqe>(&c, 1), cq, 2);
    REQUIRE(r.ok());
    CHECK_EQ(r.value().consumed, 1);
    // The cancel and its target.
    CHECK_EQ(r.value().progress.completed, 2);
    m.assert_quiesced();
}

CASE(delay_min_complete_past_what_can_arrive_returns_short)
{
    Mapped m       = Mapped::shared();
    koru_cqe cq[8] = {};
    koru_sqe s     = sqe::nop(0x2100);

    uint64_t t0   = now_ms();
    EnterResult r = m.ring.enter(std::span<const koru_sqe>(&s, 1), cq, 3);
    uint64_t dt   = now_ms() - t0;
    REQUIRE(r.ok());
    CHECK_EQ(r.value().consumed, 1);
    CHECK_EQ(r.value().progress.completed, 1);
    if (dt >= 500)
        FAILF("slept %llums waiting for what cannot arrive", (unsigned long long)dt);
    m.assert_quiesced();
}

CASE(delay_an_idle_ring_does_not_sleep_for_ever)
{
    // Unreachable means inflight == 0, not len == 0 && inflight == 0.
    Mapped m       = Mapped::shared();
    koru_cqe cq[8] = {};
    uint64_t t0    = now_ms();
    EnterResult r  = m.ring.reap(cq, 1);
    uint64_t dt    = now_ms() - t0;
    REQUIRE(r.ok());
    CHECK_EQ(r.value().progress.completed, 0);
    if (dt >= 200)
        FAILF("an idle ring slept %llums", (unsigned long long)dt);
}

CASE(delay_rejection_matrix)
{
    Mapped m   = Mapped::shared();
    koru_sqe s = sqe::delay_ns(0x2200, MS);
    s.len      = 1;
    if (m.run_one(s) != -EINVAL)
        FAILF("len");
    s        = sqe::delay_ns(0x2201, MS);
    s.handle = 9;
    if (m.run_one(s) != -EINVAL)
        FAILF("handle");
    s      = sqe::delay_ns(0x2202, MS);
    s.slot = 1;
    if (m.run_one(s) != -EINVAL)
        FAILF("slot");
    m.assert_quiesced();
}

CASE(delay_the_cap_is_accepted_and_one_past_it_is_not)
{
    Mapped m              = Mapped::shared();
    result<koru_params> p = m.ring.get_params();
    REQUIRE(p.ok());
    uint64_t cap = p.value().max_delay_ns;
    CHECK(cap > 0);

    koru_cqe cq[4] = {};
    koru_sqe s     = sqe::delay_ns(0x2300, cap);
    EnterResult r  = m.ring.enter(std::span<const koru_sqe>(&s, 1), {}, 0);
    REQUIRE(r.ok());
    // A delay at the cap is accepted.
    CHECK_EQ(r.value().consumed, 1);

    koru_sqe c = sqe::cancel(0x2301, 0x2300);
    r          = m.ring.enter(std::span<const koru_sqe>(&c, 1), cq, 2);
    REQUIRE(r.ok());
    // And can be cancelled rather than waited out.
    CHECK_EQ(r.value().progress.completed, 2);

    CHECK_EQ(m.run_one(sqe::delay_ns(0x2302, cap + 1)), -EINVAL);
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T5 - signals during a blocking ENTER
// ---------------------------------------------------------------------------

namespace {

void noop_handler(int)
{
}

/// Fork a child that blocks in ENTER on a long delay, signal it, and return
/// its wait status.
///
/// The child body is async-signal-safe by construction: no printf, no
/// assertion, and it leaves through `_exit` so the parent's buffers are never
/// flushed twice. Its verdict travels as an exit code.
int blocked_child(int sig, uint64_t delay_ns, bool handler)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        FAILF("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        FAILF("fork");
        return -1;
    }
    if (pid == 0) {
        int code;
        if (handler)
            signal(SIGINT, noop_handler);
        ::close(pipefd[0]);
        koru::SetupConfig cfg;
        cfg.sq_entries   = 32;
        cfg.cq_entries   = 64;
        cfg.slot_size    = 4096;
        cfg.slot_count   = 8;
        cfg.handle_count = 8;
        result<Ring> r   = Ring::with_config(cfg);
        if (!r) {
            code = 2;
        } else {
            Ring ring      = std::move(r).take();
            koru_cqe cq[4] = {};
            // Readiness first: the parent waits for this before signalling.
            ssize_t n = write(pipefd[1], "x", 1);
            (void)n;
            koru_sqe s    = sqe::delay_ns(0x7000, delay_ns);
            EnterResult e = ring.enter(std::span<const koru_sqe>(&s, 1), cq, 1);
            if (e.ok())
                code = 3;
            else if (e.error().err != Errno(EINTR))
                code = 4;
            else if (e.error().progress.submitted != 1)
                code = 5;
            else
                code = 0;
        }
        _exit(code);
    }

    ::close(pipefd[1]);
    char b = 0;
    if (read(pipefd[0], &b, 1) != 1)
        FAILF("child readiness");
    sleep_ms(150);
    kill(pid, sig);

    int status = 0;
    waitpid(pid, &status, 0);
    ::close(pipefd[0]);
    return status;
}

} // namespace

CASE(signals_sigint_during_enter_is_eintr_with_submitted_intact)
{
    arm_alarm(60);
    int status = blocked_child(SIGINT, 30000 * MS, true);
    disarm_alarm();
    // Child was killed, not interrupted, if this fails.
    CHECK(WIFEXITED(status));
    // 2 setup, 3 unexpected success, 4 wrong errno, 5 submitted not written back.
    CHECK_EQ(WEXITSTATUS(status), 0);
}

CASE(signals_sigkill_during_enter_kills_the_task_with_no_d_state)
{
    arm_alarm(60);
    int status = blocked_child(SIGKILL, 60000 * MS, false);
    disarm_alarm();
    CHECK(WIFSIGNALED(status)); // the child survived SIGKILL if this fails
    CHECK_EQ(WTERMSIG(status), SIGKILL);
}

// ---------------------------------------------------------------------------
// T6 - teardown torture
// ---------------------------------------------------------------------------

CASE(ringchurn_three_hundred_lifecycles_most_with_work_still_queued)
{
    ensure_pattern_file();
    int in_flight = 0;

    for (uint32_t i = 0; i < 300; i++) {
        koru::SetupConfig cfg;
        cfg.sq_entries   = 32;
        cfg.cq_entries   = 64;
        cfg.slot_size    = 4096;
        cfg.slot_count   = 4 + i % 5;
        cfg.handle_count = 8;
        Mapped m         = Mapped::make(cfg);
        if (i % 3 == 0) {
            koru_sqe sq[2] = { sqe::nop(0x10), sqe::checksum(0x11, 0, 0, 4096) };
            koru_cqe cq[2] = {};
            EnterResult r  = m.ring.enter(sq, cq, 2);
            if (!r) {
                FAILF("round %u: ENTER", i);
                return;
            }
            if (r.value().consumed != 2 || r.value().progress.completed != 2) {
                FAILF("round %u: consumed %u completed %u", i, r.value().consumed,
                      r.value().progress.completed);
                return;
            }
        } else if (i % 3 == 1) {
            int64_t h = m.open_path(0, PATFILE, KORU_O_RDONLY);
            if (h <= 0) {
                FAILF("round %u: OPEN gave %lld", i, (long long)h);
                return;
            }
            koru_sqe s    = sqe::read(0x12, uint32_t(h), 1, 0, 4096);
            EnterResult r = m.ring.enter(std::span<const koru_sqe>(&s, 1), {}, 0);
            if (!r || r.value().consumed != 1) {
                FAILF("round %u: the READ was not consumed", i);
                return;
            }
            in_flight++;
        } else {
            koru_sqe s    = sqe::delay_ns(0x13, 500 * MS);
            EnterResult r = m.ring.enter(std::span<const koru_sqe>(&s, 1), {}, 0);
            if (!r || r.value().consumed != 1) {
                FAILF("round %u: the delay was not consumed", i);
                return;
            }
            in_flight++;
        }
        // Destroyed here: munmap then close, with work still queued.
    }
    if (in_flight < 150)
        FAILF("only %d torn down with work queued", in_flight);
}

// ---------------------------------------------------------------------------
// T7 - the arena
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t M_SLOT  = 4096;
constexpr uint32_t M_COUNT = 8;
constexpr size_t M_ARENA   = size_t(M_SLOT) * M_COUNT;

koru::SetupConfig mmap_config()
{
    koru::SetupConfig c;
    c.sq_entries   = 32;
    c.cq_entries   = 64;
    c.slot_size    = M_SLOT;
    c.slot_count   = M_COUNT;
    c.handle_count = 8;
    return c;
}

/// A raw mmap attempt, so the rejection matrix can ask for things the RAII
/// wrapper will not express. -1 means it failed and errno says why.
void *try_mmap(const Ring &ring, size_t len, int flags, off_t offset)
{
    return ::mmap(nullptr, len, PROT_READ | PROT_WRITE, flags, ring.fd(), offset);
}

void expect_mmap_errno(void *p, int want, const char *what)
{
    if (p != MAP_FAILED) {
        FAILF("%s: mmap succeeded, expected errno %d", what, want);
        ::munmap(p, M_ARENA);
        return;
    }
    if (errno != want)
        FAILF("%s: errno %d, want %d", what, errno, want);
}

bool region_in_maps()
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "/dev/koru"))
            found = true;
    fclose(f);
    return found;
}

} // namespace

CASE(mmap_rejection_matrix)
{
    {
        result<Ring> u = Ring::open();
        REQUIRE(u.ok());
        Ring unconfigured = std::move(u).take();
        expect_mmap_errno(try_mmap(unconfigured, M_ARENA, MAP_SHARED, 0), EINVAL,
                          "mmap before SETUP");
    }

    result<Ring> r = Ring::with_config(mmap_config());
    REQUIRE(r.ok());
    Ring ring = std::move(r).take();

    // MAP_PRIVATE would silently give copy-on-write, the failure worth
    // guarding hardest.
    expect_mmap_errno(try_mmap(ring, M_ARENA, MAP_PRIVATE, 0), EINVAL, "MAP_PRIVATE");
    expect_mmap_errno(try_mmap(ring, M_ARENA - 4096, MAP_SHARED, 0), EINVAL, "one page short");
    expect_mmap_errno(try_mmap(ring, M_ARENA + 4096, MAP_SHARED, 0), EINVAL, "one page long");
    expect_mmap_errno(try_mmap(ring, M_ARENA, MAP_SHARED, 4096), EINVAL, "non-zero offset");

    result<Arena> a = ring.mmap();
    REQUIRE(a.ok()); // an exact MAP_SHARED mapping
    Arena arena = std::move(a).take();

    result<Arena> second = ring.mmap();
    if (second.ok())
        FAILF("a second mmap succeeded");
    else
        CHECK_EQ(second.error().raw().value, EBUSY);
    CHECK(region_in_maps()); // the region shows in /proc/self/maps
}

CASE(mmap_is_not_inherited_across_fork)
{
    result<Ring> r = Ring::with_config(mmap_config());
    REQUIRE(r.ok());
    Ring ring       = std::move(r).take();
    result<Arena> a = ring.mmap();
    REQUIRE(a.ok());
    Arena arena = std::move(a).take();

    // VM_DONTCOPY is permanent: MADV_DOFORK refuses on VM_SPECIAL, which
    // VM_MIXEDMAP and VM_DONTEXPAND already put us in.
    CHECK(!arena.madvise(MADV_DOFORK).ok());

    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0)
        _exit(region_in_maps() ? 1 : 0);
    int status = 0;
    waitpid(pid, &status, 0);
    // The child inherited it if this fails.
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

CASE(mmap_munmap_with_an_op_in_flight)
{
    result<Ring> r = Ring::with_config(mmap_config());
    REQUIRE(r.ok());
    Ring ring       = std::move(r).take();
    result<Arena> a = ring.mmap();
    REQUIRE(a.ok());
    Arena arena = std::move(a).take();

    koru_sqe s    = sqe::delay_ns(0xd1, 100 * MS);
    EnterResult e = ring.enter(std::span<const koru_sqe>(&s, 1), {}, 0);
    REQUIRE(e.ok());
    CHECK_EQ(e.value().consumed, 1); // a delay is in flight
    CHECK(arena.unmap().ok());       // munmap with an op in flight

    result<uint32_t, EnterError> left = ring.quiesce();
    REQUIRE(left.ok());
    CHECK_EQ(left.value(), 1); // the op still lands afterwards
}

CASE(mmap_survives_close_of_the_ring_fd)
{
    // vm_insert_page takes its own reference, so the mapping outlives the fd.
    // Arena deliberately does not borrow Ring, which is what lets this be
    // written at all.
    result<Ring> r = Ring::with_config(mmap_config());
    REQUIRE(r.ok());
    Ring ring       = std::move(r).take();
    result<Arena> a = ring.mmap();
    REQUIRE(a.ok());
    Arena arena = std::move(a).take();

    std::vector<uint8_t> want(M_SLOT);
    for (size_t i = 0; i < want.size(); i++)
        want[i] = pattern_byte(i);
    memcpy(arena.slot(2).data(), want.data(), want.size());

    CHECK(ring.close().ok());
    // The mapping still reads after close(fd).
    CHECK_EQ(memcmp(arena.slot(2).data(), want.data(), want.size()), 0);
    CHECK(arena.unmap().ok()); // munmap after close(fd)
}

// ---------------------------------------------------------------------------
// T7 - CHECKSUM
// ---------------------------------------------------------------------------

CASE(checksum_matches_fnv1a_on_the_host)
{
    Mapped m = Mapped::shared();
    size_t n = m.slot_size();
    std::vector<uint8_t> pattern(n);
    for (size_t i = 0; i < n; i++)
        pattern[i] = uint8_t(i * 31 + 7);
    int64_t want = fnv1a(pattern);

    for (uint32_t slot : { 0u, 3u, m.slot_count() - 1 }) {
        memcpy(m.slot(slot).data(), pattern.data(), n);
        if (m.run_one(sqe::checksum(0x40, slot, 0, uint32_t(n))) != want)
            FAILF("slot %u", slot);
    }
    // An untouched slot.
    CHECK(m.run_one(sqe::checksum(0x41, 2, 0, uint32_t(n))) != want);

    std::vector<uint8_t> rot(n);
    for (size_t i = 0; i < n; i++)
        rot[i] = pattern[(i + 1) % n];
    memcpy(m.slot(1).data(), rot.data(), n);
    // A rotated pattern.
    CHECK_EQ(m.run_one(sqe::checksum(0x42, 1, 0, uint32_t(n))), fnv1a(rot));

    // A sub-range.
    CHECK_EQ(m.run_one(sqe::checksum(0x43, 3, 100, 1000)),
             fnv1a(std::span<const uint8_t>(pattern).subspan(100, 1000)));
    // A zero-length checksum.
    CHECK_EQ(m.run_one(sqe::checksum(0x44, 3, 0, 0)), fnv1a({}));
    m.assert_quiesced();
}

CASE(checksum_rejection_matrix)
{
    Mapped m   = Mapped::shared();
    uint64_t n = m.slot_size();
    // off + len past the slot.
    CHECK_EQ(m.run_one(sqe::checksum(0x45, 0, n - 8, 16)), -EINVAL);
    // off + len overflows.
    CHECK_EQ(m.run_one(sqe::checksum(0x46, 0, UINT64_MAX, 16)), -EINVAL);
    koru_sqe s = sqe::checksum(0x47, 0, 0, 16);
    s.handle   = 1;
    if (m.run_one(s) != -EINVAL)
        FAILF("handle");
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T8 - kernel-enforced slot exclusivity
// ---------------------------------------------------------------------------

namespace {

/// 64 KB so a deferred CHECKSUM cannot finish between two dispatches in one
/// submit loop, which is what makes a collision deterministic rather than a
/// race the test usually wins.
constexpr uint32_t S_SLOT = 65536;
/// 80 slots so the busy bitmap spans two 64-bit words.
constexpr uint32_t S_COUNT = 80;

koru::SetupConfig slots_config()
{
    koru::SetupConfig c;
    c.sq_entries   = 32;
    c.cq_entries   = 64;
    c.slot_size    = S_SLOT;
    c.slot_count   = S_COUNT;
    c.handle_count = 8;
    return c;
}

/// Submit n CHECKSUMs naming these slots in one batch and return their results
/// in submission order.
std::vector<int64_t> checksum_batch(const Mapped &m, std::span<const uint32_t> slots, uint32_t len)
{
    std::vector<koru_sqe> sq;
    for (size_t i = 0; i < slots.size(); i++)
        sq.push_back(sqe::checksum(0xaa00 + i, slots[i], 0, len));
    std::vector<koru_cqe> cq(slots.size());
    EnterResult r = m.ring.enter(sq, cq, uint32_t(slots.size()));
    std::vector<int64_t> out;
    if (!r) {
        FAILF("ENTER: %s", r.error().err.message());
        return out;
    }
    CHECK_EQ(r.value().consumed, slots.size());           // all consumed
    CHECK_EQ(r.value().progress.completed, slots.size()); // all completed
    for (size_t i = 0; i < slots.size(); i++)
        out.push_back(find_cqe(cq, 0xaa00 + i).res);
    return out;
}

/// `n` CHECKSUMs on one slot, rounded, reporting how many were refused each
/// round. Which of them lose depends on how fast the kworker is, so only the
/// *best* round is a fact about the bitmap. Every round is checked for the
/// thing that must always hold: a result is either the right checksum or
/// EBUSY, never a wrong answer.
size_t collisions(const Mapped &m, uint32_t slot, size_t n, int64_t want)
{
    size_t best = 0;
    for (int round = 0; round < 64; round++) {
        std::vector<uint32_t> slots(n, slot);
        std::vector<int64_t> res = checksum_batch(m, slots, S_SLOT);
        size_t busy              = 0;
        for (int64_t r : res) {
            if (r == -EBUSY)
                busy++;
            else if (r != want)
                FAILF("a result that is neither the checksum nor EBUSY: %lld", (long long)r);
        }
        if (busy > best)
            best = busy;
    }
    return best;
}

} // namespace

CASE(slots_one_op_wins_and_the_rest_get_ebusy)
{
    Mapped m = Mapped::make(slots_config());
    std::vector<uint8_t> pattern(S_SLOT);
    for (size_t i = 0; i < pattern.size(); i++)
        pattern[i] = uint8_t(i * 17 + 3);
    int64_t want = fnv1a(pattern);
    memcpy(m.slot(3).data(), pattern.data(), pattern.size());

    // One of two gets -EBUSY.
    CHECK_EQ(collisions(m, 3, 2, want), 1);

    // Released in the same critical section that posts the CQE, so it is free
    // exactly when userspace can see the completion.
    CHECK_EQ(m.run_one(sqe::checksum(0xab, 3, 0, S_SLOT)), want);

    // Two of three get -EBUSY.
    CHECK_EQ(collisions(m, 3, 3, want), 2);
    m.assert_quiesced();
}

CASE(slots_distinct_slots_never_collide)
{
    Mapped m = Mapped::make(slots_config());

    {
        const uint32_t pair[] = { 3, 4 };
        for (int64_t r : checksum_batch(m, pair, S_SLOT))
            if (r == -EBUSY)
                FAILF("slots 3 and 4 collided");
    }
    // Slot 70 lives in the second bitmap word. Its contents are whatever the
    // arena was zeroed to, which is all `collisions` needs to compare against.
    std::vector<uint8_t> zeros(S_SLOT, 0);
    CHECK_EQ(collisions(m, 70, 2, fnv1a(zeros)), 1); // slot 70 is tracked too
    {
        const uint32_t pair[] = { 6, 70 };
        for (int64_t r : checksum_batch(m, pair, S_SLOT))
            if (r == -EBUSY)
                FAILF("6 and 70 alias");
    }
    {
        std::vector<uint32_t> all;
        for (uint32_t i = 0; i < 8; i++)
            all.push_back(i);
        for (int64_t r : checksum_batch(m, all, S_SLOT))
            if (r == -EBUSY)
                FAILF("eight distinct slots collided");
    }
    m.assert_quiesced();
}

CASE(slots_out_of_range_indices_are_rejected)
{
    // slot_try_acquire does not bounds-check itself, so a missing guard here
    // is a Rust bounds panic in the kernel. The bitmap is whole 64-bit words,
    // so slot_count + 1 is still inside it.
    Mapped m = Mapped::make(slots_config());
    for (uint32_t slot : { S_COUNT, S_COUNT + 1, UINT32_MAX })
        if (m.run_one(sqe::checksum(0xac, slot, 0, 16)) != -EINVAL)
            FAILF("slot %u", slot);
    // Rejected before it claims, so the slot stays usable.
    CHECK_EQ(m.run_one(sqe::checksum(0xad, 5, S_SLOT, 16)), -EINVAL);
    CHECK(m.run_one(sqe::checksum(0xae, 5, 0, S_SLOT)) >= 0);
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T9 - OPEN, CLOSE and the generational handle table
// ---------------------------------------------------------------------------

namespace {

uint32_t mode_of(const char *path)
{
    struct stat st = {};
    if (stat(path, &st) < 0)
        return 0;
    return st.st_mode & 07777;
}

bool write_file(const char *path, const char *bytes, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(bytes, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

int64_t size_of(const char *path)
{
    struct stat st = {};
    if (stat(path, &st) < 0)
        return -1;
    return st.st_size;
}

} // namespace

CASE(open_handle_matrix)
{
    Mapped m = Mapped::shared();

    int64_t h = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
    if (h <= 0) {
        FAILF("OPEN gave %lld", (long long)h);
        return;
    }
    uint32_t idx = KORU_HANDLE_INDEX(h), generation = KORU_HANDLE_GEN(h);
    CHECK(generation != 0);        // a generation must never be 0
    CHECK(idx < m.handle_count()); // the index is inside the table

    CHECK_EQ(m.close_handle(uint32_t(h)), 0);
    CHECK_EQ(m.close_handle(uint32_t(h)), -EBADF); // a double CLOSE
    CHECK_EQ(m.close_handle(0), -EBADF);           // handle 0
    // A stale generation.
    CHECK_EQ(m.close_handle(KORU_MAKE_HANDLE(idx, uint16_t(generation + 7))), -EBADF);
    // An index past the table.
    CHECK_EQ(m.close_handle(KORU_MAKE_HANDLE(m.handle_count(), 1)), -EBADF);

    // A reused index must come back with a new generation, which is what makes
    // the retired handle reject.
    int64_t h2 = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
    REQUIRE(h2 > 0);
    CHECK_EQ(KORU_HANDLE_INDEX(h2), idx);          // the index is reused
    CHECK(KORU_HANDLE_GEN(h2) != generation);      // with a new generation
    CHECK_EQ(m.close_handle(uint32_t(h)), -EBADF); // the old handle is stale
    CHECK_EQ(m.close_handle(uint32_t(h2)), 0);
    m.assert_quiesced();
}

CASE(open_close_rejects_fields_it_does_not_read)
{
    Mapped m  = Mapped::shared();
    int64_t h = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
    REQUIRE(h > 0);
    koru_sqe s = sqe::close(0x50, uint32_t(h));
    s.len      = 1;
    if (m.run_one(s) != -EINVAL)
        FAILF("len");
    s     = sqe::close(0x51, uint32_t(h));
    s.off = 1;
    if (m.run_one(s) != -EINVAL)
        FAILF("off");
    s      = sqe::close(0x52, uint32_t(h));
    s.slot = 1;
    if (m.run_one(s) != -EINVAL)
        FAILF("slot");
    // And the handle survived all three.
    CHECK_EQ(m.close_handle(uint32_t(h)), 0);
    m.assert_quiesced();
}

CASE(open_path_matrix)
{
    Mapped m   = Mapped::shared();
    uint32_t n = m.slot_size();

    CHECK_EQ(m.open_path(0, "/no/such/path/here", KORU_O_RDONLY), -ENOENT);

    // An embedded NUL in the path bytes.
    uint32_t len = m.put_path(0, "/etc/hostname");
    m.slot(0)[3] = 0;
    CHECK_EQ(m.run_one(sqe::open(0x60, 0, 0, len, KORU_O_RDONLY)), -EINVAL);

    m.put_path(0, "/etc/hostname");
    // A zero-length path.
    CHECK_EQ(m.run_one(sqe::open(0x61, 0, 0, 0, KORU_O_RDONLY)), -EINVAL);
    // len past the slot.
    CHECK_EQ(m.run_one(sqe::open(0x62, 0, 0, n + 1, KORU_O_RDONLY)), -EINVAL);
    // off + len past the slot.
    CHECK_EQ(m.run_one(sqe::open(0x63, 0, n - 4, 8, KORU_O_RDONLY)), -EINVAL);
    // slot past the arena.
    CHECK_EQ(m.run_one(sqe::open(0x64, m.slot_count(), 0, 8, KORU_O_RDONLY)), -EINVAL);

    // PATH_MAX is the boundary: 4095 resolves and fails, 4096 is refused.
    std::string longp;
    for (int i = 0; i < 2047; i++)
        longp += "/a";
    longp += "/";
    CHECK_EQ(longp.size(), 4095);
    // A PATH_MAX-1 path resolves.
    CHECK_EQ(m.open_path(0, longp.c_str(), KORU_O_RDONLY), -ENOENT);
    std::string longer = longp + "a";
    // A PATH_MAX path is refused.
    CHECK_EQ(m.open_path(0, longer.c_str(), KORU_O_RDONLY), -EINVAL);
    m.assert_quiesced();
}

CASE(open_flag_matrix)
{
    Mapped m = Mapped::shared();

    // An unknown open flag bit.
    CHECK_EQ(m.open_path(0, "/etc/hostname", 1u << 9), -EINVAL);
    // Access mode 3.
    CHECK_EQ(m.open_path(0, "/etc/hostname", KORU_O_ACCMODE), -EINVAL);

    int64_t h = m.open_path(0, "/etc", KORU_O_RDONLY | KORU_O_DIRECTORY);
    CHECK(h > 0); // a directory with KORU_O_DIRECTORY
    CHECK_EQ(m.close_handle(uint32_t(h)), 0);

    CHECK_EQ(m.open_path(0, "/etc/hostname", KORU_O_DIRECTORY), -ENOTDIR);
    CHECK_EQ(m.open_path(0, "/etc", KORU_O_WRONLY), -EISDIR);
    // Since T18 OPEN gates on nothing: READ and WRITE carry the type rule.
    int64_t d = m.open_path(0, "/dev/null", KORU_O_RDONLY);
    CHECK(d > 0);                                          // a device node yields a handle
    CHECK_EQ(m.read_into(uint32_t(d), 1, 0, 64), -EINVAL); // but READ refuses it
    CHECK_EQ(m.close_handle(uint32_t(d)), 0);

    const char *link = "/tmp/koru-check-cpp-symlink";
    ::unlink(link);
    if (symlink("/etc/hostname", link) == 0) {
        h = m.open_path(0, link, KORU_O_RDONLY);
        CHECK(h > 0); // a symlink is followed by default
        CHECK_EQ(m.close_handle(uint32_t(h)), 0);
        CHECK_EQ(m.open_path(0, link, KORU_O_NOFOLLOW), -ELOOP);
        ::unlink(link);
    } else {
        test_skip("open_flag_matrix symlink", "could not create the symlink");
    }
    m.assert_quiesced();
}

CASE(open_holds_its_slot_only_for_the_path_snapshot)
{
    Mapped m     = Mapped::shared();
    uint32_t len = m.put_path(1, "/etc/hostname");
    uint32_t n   = m.slot_size();

    std::vector<Res> res = m.slot_race(64, [&](uint64_t a, uint64_t b, koru_sqe *sq) {
        sq[0] = sqe::checksum(a, 1, 0, n);
        sq[1] = sqe::open(b, 1, 0, len, KORU_O_RDONLY);
    });
    bool any_busy        = false;
    for (const Res &r : res)
        any_busy = any_busy || r.res == -EBUSY;
    CHECK(any_busy); // the slot was never held across the OPEN behind it

    // The only two outcomes: refused the slot, or given a real handle.
    for (const Res &r : res) {
        if (r.res == -EBUSY)
            continue;
        if (r.res <= 0) {
            FAILF("the OPEN got neither the slot nor a handle: %lld", (long long)r.res);
            continue;
        }
        CHECK_EQ(m.close_handle(uint32_t(r.res)), 0);
    }
    m.assert_quiesced();
}

/// The creation flags, and the mode argument after the path.
CASE(open_creation_matrix)
{
    Mapped m         = Mapped::shared();
    const char *path = "/tmp/koru-check-cpp-created";
    ::unlink(path);

    int64_t h = m.create_path(0, path, KORU_O_WRONLY | KORU_O_CREAT, 0640);
    CHECK(h > 0); // O_CREAT of a missing path
    CHECK_EQ(m.close_handle(uint32_t(h)), 0);
    CHECK_EQ(access(path, F_OK), 0); // the file is there
    CHECK_EQ(mode_of(path), 0640);   // the mode asked for

    // Without O_EXCL an existing path is opened, not refused.
    h = m.create_path(0, path, KORU_O_WRONLY | KORU_O_CREAT, 0600);
    CHECK(h > 0);
    CHECK_EQ(m.close_handle(uint32_t(h)), 0);
    CHECK_EQ(mode_of(path), 0640); // and leaves its mode alone

    // O_CREAT|O_EXCL over an existing path.
    CHECK_EQ(m.create_path(0, path, KORU_O_WRONLY | KORU_O_CREAT | KORU_O_EXCL, 0640), -EEXIST);
    // O_EXCL without O_CREAT.
    CHECK_EQ(m.open_path(0, path, KORU_O_WRONLY | KORU_O_EXCL), -EINVAL);
    // A setuid creation mode.
    CHECK_EQ(m.create_path(0, path, KORU_O_WRONLY | KORU_O_CREAT, 04755), -EINVAL);
    // A mode above the mask.
    CHECK_EQ(m.create_path(0, path, KORU_O_WRONLY | KORU_O_CREAT, uint64_t(1) << 32), -EINVAL);

    // The mode is read only when O_CREAT asked for one, so an open of an
    // existing path needs no room for it.
    uint32_t n   = m.put_path(0, path);
    uint64_t end = m.slot_size() - n;
    memmove(m.slot(0).data() + end, m.slot(0).data(), n);
    h = m.run_one(sqe::open(0x120, 0, end, n, KORU_O_RDONLY));
    CHECK(h > 0); // a path at the very end of the slot, without O_CREAT
    CHECK_EQ(m.close_handle(uint32_t(h)), 0);
    // And with O_CREAT there is no room for the mode.
    CHECK_EQ(m.run_one(sqe::open(0x121, 0, end, n, KORU_O_RDONLY | KORU_O_CREAT)), -EINVAL);

    // O_TRUNC and O_APPEND, over the file just made.
    CHECK(write_file(path, "12345678", 8));
    h = m.open_path(0, path, KORU_O_WRONLY | KORU_O_TRUNC);
    CHECK(h > 0); // O_TRUNC opens
    CHECK_EQ(m.close_handle(uint32_t(h)), 0);
    CHECK_EQ(size_of(path), 0); // and empties it

    m.fill(1, [](size_t) { return uint8_t('a'); });
    h = m.open_path(0, path, KORU_O_WRONLY | KORU_O_APPEND);
    REQUIRE(h > 0); // O_APPEND opens
    // off is ignored on an appending handle, so two writes at 0 stack up.
    CHECK_EQ(m.write_from(uint32_t(h), 1, 0, 4), 4);
    CHECK_EQ(m.write_from(uint32_t(h), 1, 0, 4), 4);
    CHECK_EQ(m.close_handle(uint32_t(h)), 0);
    CHECK_EQ(size_of(path), 8); // eight bytes, not four

    ::unlink(path);
    m.assert_quiesced();
}

CASE(open_exhausts_the_handle_table_with_emfile)
{
    koru::SetupConfig cfg;
    cfg.sq_entries   = 32;
    cfg.cq_entries   = 64;
    cfg.slot_size    = 8192;
    cfg.slot_count   = 4;
    cfg.handle_count = 8;
    Mapped m         = Mapped::make(cfg);

    uint32_t n = m.handle_count();
    std::vector<int64_t> held;
    for (uint32_t i = 0; i < n; i++)
        held.push_back(m.open_path(0, "/etc/hostname", KORU_O_RDONLY));
    for (int64_t h : held)
        if (h <= 0)
            FAILF("the whole table could not be held at once: %lld", (long long)h);
    // One past it.
    CHECK_EQ(m.open_path(0, "/etc/hostname", KORU_O_RDONLY), -EMFILE);

    REQUIRE(held.size() > 3);
    uint32_t retired = uint32_t(held[3]);
    CHECK_EQ(m.close_handle(retired), 0);
    int64_t fresh = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
    REQUIRE(fresh > 0);
    // The index is reused.
    CHECK_EQ(KORU_HANDLE_INDEX(fresh), KORU_HANDLE_INDEX(retired));
    // The retired handle stays stale.
    CHECK_EQ(m.close_handle(retired), -EBADF);
    m.assert_quiesced();
}

CASE(handles_five_hundred_open_close_pairs_leak_nothing)
{
    size_t fds_before    = count_fds();
    int64_t files_before = file_nr_settled();
    {
        Mapped m = Mapped::shared();
        for (int i = 0; i < 500; i++) {
            int64_t h = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
            if (h <= 0) {
                FAILF("OPEN gave %lld", (long long)h);
                return;
            }
            if (m.close_handle(uint32_t(h)) != 0) {
                FAILF("CLOSE refused");
                return;
            }
        }
    }

    // filp_open installs no descriptor, so /proc/<pid>/fd sees nothing either
    // way; field 1 of file-nr is the instrument that can.
    CHECK_EQ(count_fds(), fds_before);
    int64_t leaked = file_nr_settled() - files_before;
    if (leaked >= 64)
        FAILF("%lld struct files leaked", (long long)leaked);
}

CASE(handles_release_drains_the_table_at_close_not_after_the_delay)
{
    int64_t before = file_nr_settled();
    int64_t during = 0;
    uint64_t dt    = 0;
    {
        koru::SetupConfig cfg;
        cfg.sq_entries   = 32;
        cfg.cq_entries   = 64;
        cfg.slot_size    = 8192;
        cfg.slot_count   = 4;
        cfg.handle_count = 8;
        Mapped m         = Mapped::make(cfg);
        uint32_t len     = m.put_path(0, "/etc/hostname");

        koru_sqe sq[2] = { sqe::delay_ns(0xb0, 2000 * MS),
                           sqe::open(0xb1, 0, 0, len, KORU_O_RDONLY) };
        koru_cqe cq[4] = {};
        EnterResult r  = m.ring.enter(sq, cq, 1);
        REQUIRE(r.ok());
        CHECK_EQ(r.value().consumed, 2);
        // The OPEN lands, the delay is still queued.
        CHECK_EQ(r.value().progress.completed, 1);
        // A handle opened with a 2s delay still queued.
        CHECK(find_cqe(std::span<const koru_cqe>(cq, 1), 0xb1).res > 0);

        during      = file_nr_settled();
        uint64_t t0 = now_ms();
        m.arena.unmap();
        m.ring.close();
        dt = now_ms() - t0;
    }
    // The handle and the ring fd are both counted.
    if (during < before + 2)
        FAILF("file-nr went from %lld to %lld", (long long)before, (long long)during);
    if (dt >= 500)
        FAILF("close took %llums: the delay was waited out, not cancelled", (unsigned long long)dt);
    // Closing the ring drops the handle at once.
    CHECK(file_nr_settled() <= before);
}

// ---------------------------------------------------------------------------
// T9 - credentials
// ---------------------------------------------------------------------------

namespace {

/// nobody's uid and gid from /etc/passwd, so the forked child needs no
/// allocating libc call of its own.
void nobody_ids(uint32_t &uid, uint32_t &gid)
{
    uid     = 65534;
    gid     = 65534;
    FILE *f = fopen("/etc/passwd", "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "nobody:", 7) != 0)
            continue;
        unsigned u = 0, g = 0;
        if (sscanf(line, "nobody:%*[^:]:%u:%u:", &u, &g) == 2) {
            uid = u;
            gid = g;
        }
        break;
    }
    fclose(f);
}

} // namespace

CASE(creds_an_unprivileged_submitter_gets_eacces)
{
    // OPEN runs inline in the submitting task, so the check uses the caller's
    // credentials. Deferred to a kworker it would resolve as root.
    if (!is_root()) {
        test_skip("creds", "not root");
        return;
    }
    struct stat st = {};
    if (stat("/etc/shadow", &st) < 0) {
        test_skip("creds", "no /etc/shadow");
        return;
    }
    if (st.st_mode & 0004) {
        test_skip("creds", "/etc/shadow is world-readable");
        return;
    }

    uint32_t uid = 0, gid = 0;
    nobody_ids(uid, gid);

    // The arena is VM_DONTCOPY, so the parent must not map: the child maps it
    // itself after the fork.
    koru::SetupConfig cfg;
    cfg.sq_entries   = 32;
    cfg.cq_entries   = 64;
    cfg.slot_size    = 8192;
    cfg.slot_count   = 4;
    cfg.handle_count = 8;
    result<Ring> r   = Ring::with_config(cfg);
    REQUIRE(r.ok());
    Ring ring = std::move(r).take();

    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        int code;
        result<Arena> a = ring.mmap();
        if (!a) {
            code = 2;
        } else {
            setgroups(0, nullptr);
            if (setresgid(gid, gid, gid) != 0 || setresuid(uid, uid, uid) != 0)
                code = 3;
            else if (geteuid() == 0)
                code = 4;
            else {
                Mapped m;
                m.ring  = std::move(ring);
                m.arena = std::move(a).take();
                if (m.open_path(0, "/etc/shadow", KORU_O_RDONLY) != -EACCES)
                    code = 5;
                else if (m.open_path(0, "/etc/hostname", KORU_O_RDONLY) <= 0)
                    code = 6;
                else
                    code = 0;
            }
        }
        _exit(code);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status)); // the child died otherwise
    // 2 mmap, 3 setresuid, 4 still root, 5 shadow not EACCES, 6 hostname failed.
    CHECK_EQ(WEXITSTATUS(status), 0);
}

// ---------------------------------------------------------------------------
// T10 - READ into a slot
// ---------------------------------------------------------------------------

namespace {

/// Whole file, or empty where it could not be read.
std::vector<uint8_t> read_file(const char *path)
{
    std::vector<uint8_t> out;
    FILE *f = fopen(path, "rb");
    if (!f)
        return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

/// Every byte of `s` is the pattern starting at `from`.
bool is_pattern(std::span<const uint8_t> s, size_t from)
{
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] != pattern_byte(i + from))
            return false;
    return true;
}

} // namespace

CASE(read_matches_read_2_byte_for_byte)
{
    Mapped m                  = Mapped::shared();
    std::vector<uint8_t> want = read_file("/etc/hostname");
    if (want.empty()) {
        test_skip("read", "/etc/hostname is unreadable or empty");
        return;
    }
    int64_t h = m.open_path(0, "/etc/hostname", KORU_O_RDONLY);
    REQUIRE(h > 0);
    int64_t n = m.read_into(uint32_t(h), 1, 0, m.slot_size());
    CHECK_EQ(n, int64_t(want.size()));
    if (n == int64_t(want.size()))
        CHECK_EQ(memcmp(m.slot(1).data(), want.data(), want.size()), 0);
    CHECK_EQ(m.close_handle(uint32_t(h)), 0);
    m.assert_quiesced();
}

CASE(read_reproduces_the_pattern_at_any_offset)
{
    ensure_pattern_file();
    Mapped m   = Mapped::shared();
    uint32_t n = m.slot_size();
    int64_t hh = m.open_path(0, PATFILE, KORU_O_RDONLY);
    REQUIRE(hh > 0);
    uint32_t h = uint32_t(hh);

    // A multi-page READ.
    CHECK_EQ(m.read_into(h, 1, 0, n), int64_t(PATSIZE));
    CHECK(is_pattern(m.slot(1), 0));

    uint64_t off = 4097;
    uint32_t len = n - uint32_t(off);
    // An unaligned file offset.
    CHECK_EQ(m.read_into(h, 2, off, len), int64_t(len));
    CHECK(is_pattern(m.slot(2).subspan(0, len), off));

    // A short read at EOF is a result, not an error.
    CHECK_EQ(m.read_into(h, 3, PATSIZE - 100, n), 100);
    CHECK(is_pattern(m.slot(3).subspan(0, 100), PATSIZE - 100));
    CHECK_EQ(m.read_into(h, 3, PATSIZE, n), 0);        // a read at EOF
    CHECK_EQ(m.read_into(h, 3, PATSIZE + 4096, n), 0); // a read past EOF
    CHECK_EQ(m.read_into(h, 4, 123, 1), 1);            // a one-byte read
    CHECK_EQ(m.slot(4)[0], pattern_byte(123));

    CHECK_EQ(m.close_handle(h), 0);
    m.assert_quiesced();
}

CASE(read_rejection_matrix)
{
    ensure_pattern_file();
    Mapped m   = Mapped::shared();
    uint32_t n = m.slot_size();
    int64_t hh = m.open_path(0, PATFILE, KORU_O_RDONLY);
    REQUIRE(hh > 0);
    uint32_t h = uint32_t(hh);

    CHECK_EQ(m.read_into(0, 1, 0, n), -EBADF);             // handle 0
    CHECK_EQ(m.read_into(h + (1 << 16), 1, 0, n), -EBADF); // a stale generation
    // An index past the table.
    CHECK_EQ(m.read_into(KORU_MAKE_HANDLE(m.handle_count(), 1), 1, 0, n), -EBADF);
    CHECK_EQ(m.read_into(h, 1, 0, 0), -EINVAL);              // len 0
    CHECK_EQ(m.read_into(h, 1, 0, n + 1), -EINVAL);          // len past the slot
    CHECK_EQ(m.read_into(h, m.slot_count(), 0, n), -EINVAL); // slot past the arena
    // A negative file offset.
    CHECK_EQ(m.read_into(h, 1, uint64_t(1) << 63, n), -EINVAL);

    // READ holds its slot for the whole deferred op.
    std::vector<Res> res = m.slot_race(64, [&](uint64_t a, uint64_t b, koru_sqe *sq) {
        sq[0] = sqe::checksum(a, 5, 0, n);
        sq[1] = sqe::read(b, h, 5, 0, n);
    });
    bool any_busy        = false;
    for (const Res &r : res)
        any_busy = any_busy || r.res == -EBUSY;
    CHECK(any_busy); // two ops on one slot

    CHECK_EQ(m.close_handle(h), 0);
    CHECK_EQ(m.read_into(h, 1, 0, n), -EBADF); // a closed handle

    // Rejected before kernel_read can warn; the gate script greps for that text.
    int64_t d = m.open_path(0, "/etc", KORU_O_RDONLY | KORU_O_DIRECTORY);
    REQUIRE(d > 0);
    CHECK_EQ(m.read_into(uint32_t(d), 1, 0, n), -EINVAL); // READ of a directory
    CHECK_EQ(m.close_handle(uint32_t(d)), 0);
    m.assert_quiesced();
}

CASE(read_race_four_hundred_closes_against_an_in_flight_read)
{
    // The ARef<File> is resolved at submit time, so a CLOSE racing the op
    // cannot free it. 64 KB slots make the window the common case.
    ensure_pattern_file();
    arm_alarm(120);
    Mapped m        = Mapped::shared();
    uint32_t n      = m.slot_size();
    int close_first = 0;

    for (int round = 0; round < 400; round++) {
        int64_t h = m.open_path(0, PATFILE, KORU_O_RDONLY);
        if (h <= 0) {
            FAILF("round %d: OPEN gave %lld", round, (long long)h);
            return;
        }
        koru_sqe sq[2] = { sqe::read(0xb0, uint32_t(h), 1, 0, n), sqe::close(0xb1, uint32_t(h)) };
        koru_cqe cq[2] = {};
        EnterResult r  = m.ring.enter(sq, cq, 2);
        if (!r || r.value().progress.completed != 2) {
            FAILF("round %d: C1", round);
            return;
        }
        if (find_cqe(cq, 0xb0).res != int64_t(PATSIZE)) {
            FAILF("round %d: the READ", round);
            return;
        }
        if (find_cqe(cq, 0xb1).res != 0) {
            FAILF("round %d: the CLOSE", round);
            return;
        }
        if (cq[0].user_data == 0xb1)
            close_first++;
    }
    disarm_alarm();

    // The last one still read the right bytes.
    CHECK(is_pattern(m.slot(1), 0));
    if (close_first < 350)
        FAILF("the CLOSE landed first only %d times of 400", close_first);
    m.assert_quiesced();
}

// ---------------------------------------------------------------------------
// T11 - CANCEL
// ---------------------------------------------------------------------------

CASE(cancel_a_queued_delay_returns_at_once)
{
    Mapped m      = Mapped::shared();
    koru_sqe d    = sqe::delay_ns(0x10, 2000 * MS);
    EnterResult r = m.ring.enter(std::span<const koru_sqe>(&d, 1), {}, 0);
    REQUIRE(r.ok());
    CHECK_EQ(r.value().consumed, 1); // a 2s DELAY_NS is queued

    koru_cqe cq[4] = {};
    koru_sqe c     = sqe::cancel(0x11, 0x10);
    uint64_t t0    = now_ms();
    r              = m.ring.enter(std::span<const koru_sqe>(&c, 1), cq, 2);
    uint64_t dt    = now_ms() - t0;
    REQUIRE(r.ok());

    CHECK_EQ(r.value().consumed, 1);
    // The CANCEL and its target both complete (C1).
    CHECK_EQ(r.value().progress.completed, 2);
    CHECK_EQ(find_cqe(cq, 0x11).res, 0);          // the canceller
    CHECK_EQ(find_cqe(cq, 0x10).res, -ECANCELED); // the target
    if (dt >= 500)
        FAILF("took %llums: the delay was waited out", (unsigned long long)dt);

    // A second cancel.
    CHECK_EQ(m.run_one(sqe::cancel(0x12, 0x10)), -ENOENT);
    m.assert_quiesced();
}

CASE(cancel_of_something_not_in_flight_is_enoent)
{
    Mapped m = Mapped::shared();
    CHECK_EQ(m.run_one(sqe::cancel(0x20, 0xdeadbeef)), -ENOENT); // no such op
    CHECK_EQ(m.run_one(sqe::nop(0x21)), 0);
    // An op that already completed.
    CHECK_EQ(m.run_one(sqe::cancel(0x22, 0x21)), -ENOENT);
    m.assert_quiesced();
}

CASE(cancel_rejects_fields_it_does_not_read)
{
    Mapped m   = Mapped::shared();
    koru_sqe s = sqe::cancel(0x30, 1);
    s.len      = 1;
    if (m.run_one(s) != -EINVAL)
        FAILF("len");
    s      = sqe::cancel(0x31, 1);
    s.slot = 1;
    if (m.run_one(s) != -EINVAL)
        FAILF("slot");
    s        = sqe::cancel(0x32, 1);
    s.handle = 1;
    if (m.run_one(s) != -EINVAL)
        FAILF("handle");
    m.assert_quiesced();
}

CASE(cancel_releases_the_slot_the_target_held)
{
    Mapped m      = Mapped::shared();
    uint32_t n    = m.slot_size();
    koru_sqe s    = sqe::checksum(0x40, 2, 0, n);
    EnterResult r = m.ring.enter(std::span<const koru_sqe>(&s, 1), {}, 0);
    REQUIRE(r.ok());
    CHECK_EQ(r.value().consumed, 1);

    koru_cqe cq[4] = {};
    koru_sqe c     = sqe::cancel(0x41, 0x40);
    r              = m.ring.enter(std::span<const koru_sqe>(&c, 1), cq, 2);
    REQUIRE(r.ok());
    CHECK_EQ(r.value().progress.completed, 2);
    // The slot was released.
    CHECK(m.run_one(sqe::checksum(0x42, 2, 0, n)) >= 0);
    m.assert_quiesced();
}

namespace {

/// Race a CANCEL against its target, counting the three outcomes.
struct CancelRace {
    uint32_t ok = 0, already = 0, enoent = 0;
};

CancelRace cancel_race(const Mapped &m, uint64_t delay_ns, uint32_t rounds)
{
    CancelRace out;
    for (uint32_t round = 0; round < rounds; round++) {
        koru_sqe d    = sqe::delay_ns(0x30, delay_ns);
        EnterResult r = m.ring.enter(std::span<const koru_sqe>(&d, 1), {}, 0);
        if (!r || r.value().consumed != 1) {
            FAILF("round %u: the delay was not queued", round);
            return out;
        }

        koru_cqe cq[4] = {};
        koru_sqe c     = sqe::cancel(0x31, 0x30);
        r              = m.ring.enter(std::span<const koru_sqe>(&c, 1), cq, 2);
        if (!r || r.value().consumed != 1) {
            FAILF("round %u: the cancel was not consumed", round);
            return out;
        }
        // Every SQE yields exactly one CQE.
        if (r.value().progress.completed != 2) {
            FAILF("round %u: %u completions, want 2", round, r.value().progress.completed);
            return out;
        }

        int64_t res = find_cqe(cq, 0x31).res;
        if (res == 0)
            out.ok++;
        else if (res == -EALREADY)
            out.already++;
        else if (res == -ENOENT)
            out.enoent++;
        else
            FAILF("round %u: the canceller returned %lld", round, (long long)res);

        int64_t target = find_cqe(cq, 0x30).res;
        if (target != 0 && target != -ECANCELED)
            FAILF("round %u: the target returned %lld", round, (long long)target);
    }
    // Every round accounted for.
    CHECK_EQ(out.ok + out.already + out.enoent, rounds);
    return out;
}

} // namespace

CASE(cancel_race_against_an_armed_timer)
{
    arm_alarm(120);
    Mapped m     = Mapped::shared();
    CancelRace r = cancel_race(m, 2 * MS, 300);
    disarm_alarm();
    if (r.ok < 250)
        FAILF("the cancel won only %u of 300", r.ok);
    m.assert_quiesced();
}

CASE(cancel_race_against_the_worker)
{
    arm_alarm(120);
    Mapped m     = Mapped::shared();
    CancelRace r = cancel_race(m, 0, 300);
    disarm_alarm();
    if (r.enoent < 50)
        FAILF("the worker won only %u of 300", r.enoent);
    m.assert_quiesced();
}

CASE(cancel_race_against_a_running_checksum)
{
    // run() unregisters after complete(), so a cancel during execution reports
    // -EALREADY rather than -ENOENT. A whole-slot 64 KB CHECKSUM is what makes
    // that window reachable at all; it is counted but not gated, because it
    // lands one to three times per thousand.
    arm_alarm(180);
    Mapped m    = Mapped::shared();
    uint32_t n  = m.slot_size();
    uint32_t ok = 0, already = 0, enoent = 0;

    for (int round = 0; round < 1000; round++) {
        koru_sqe s    = sqe::checksum(0x50, 3, 0, n);
        EnterResult r = m.ring.enter(std::span<const koru_sqe>(&s, 1), {}, 0);
        if (!r || r.value().consumed != 1) {
            FAILF("round %d: the checksum was not queued", round);
            return;
        }
        koru_cqe cq[4] = {};
        koru_sqe c     = sqe::cancel(0x51, 0x50);
        r              = m.ring.enter(std::span<const koru_sqe>(&c, 1), cq, 2);
        if (!r || r.value().progress.completed != 2) {
            FAILF("round %d: C1 holds", round);
            return;
        }
        int64_t res = find_cqe(cq, 0x51).res;
        if (res == 0)
            ok++;
        else if (res == -EALREADY)
            already++;
        else if (res == -ENOENT)
            enoent++;
        else
            FAILF("round %d: the canceller returned %lld", round, (long long)res);
    }
    disarm_alarm();
    test_note("cancel vs running CHECKSUM: %u cancelled, %u already, %u missed", ok, already,
              enoent);
    if (ok < 150)
        FAILF("the cancel won only %u of 1000", ok);
    if (enoent < 20)
        FAILF("the CHECKSUM won only %u of 1000", enoent);
    m.assert_quiesced();
}

CASE(cancel_leak_two_hundred_open_read_cancel_close_rounds)
{
    // Skipping the reference drop CANCEL owes on a true return leaks the op
    // and wedges rmmod; doing it on a false return is a use-after-free.
    ensure_pattern_file();
    arm_alarm(120);
    int64_t before = file_nr_settled();
    {
        Mapped m   = Mapped::shared();
        uint32_t n = m.slot_size();

        for (int round = 0; round < 200; round++) {
            int64_t h = m.open_path(0, PATFILE, KORU_O_RDONLY);
            if (h <= 0) {
                FAILF("round %d: OPEN", round);
                return;
            }
            koru_sqe s    = sqe::read(0x60, uint32_t(h), 1, 0, n);
            EnterResult r = m.ring.enter(std::span<const koru_sqe>(&s, 1), {}, 0);
            if (!r || r.value().consumed != 1) {
                FAILF("round %d: the READ was not queued", round);
                return;
            }
            koru_cqe cq[4] = {};
            koru_sqe c     = sqe::cancel(0x61, 0x60);
            r              = m.ring.enter(std::span<const koru_sqe>(&c, 1), cq, 2);
            if (!r || r.value().progress.completed != 2) {
                FAILF("round %d: C1", round);
                return;
            }
            if (m.close_handle(uint32_t(h)) != 0) {
                FAILF("round %d: CLOSE", round);
                return;
            }
        }
        disarm_alarm();
        m.assert_quiesced();
    }

    int64_t leaked = file_nr_settled() - before;
    if (leaked >= 64)
        FAILF("%lld struct files leaked: an OpWork kept its file reference", (long long)leaked);
}

// ---------------------------------------------------------------------------
// T3 - SETUP and GET_PARAMS
// ---------------------------------------------------------------------------
//
// Not strictly T4-T11, but the ioctl numbers and the typed wrappers are this
// binding's, and a wrong direction bit is the likeliest defect in a
// hand-written _IOWR.

namespace {

koru_params good_request()
{
    koru::SetupConfig c;
    c.sq_entries   = 64;
    c.cq_entries   = 128;
    c.slot_size    = 4096;
    c.slot_count   = 32;
    c.handle_count = 0;
    return c.to_params();
}

} // namespace

CASE(setup_get_params_works_before_setup)
{
    result<Ring> r = Ring::open();
    REQUIRE(r.ok());
    Ring ring             = std::move(r).take();
    result<koru_params> q = ring.get_params();
    REQUIRE(q.ok());
    const koru_params &p = q.value();
    CHECK_EQ(p.magic, KORU_MAGIC);
    CHECK_EQ(p.abi_version, KORU_ABI_VERSION);
    CHECK_EQ(p.configured, 0);
    CHECK_EQ(p.handle_count, 0);
    // Exact, not merely non-zero. scripts/abi.sh diffs the two userspace
    // mirrors against each other and cannot see the kernel; this is the only
    // assertion tying either of them to the canonical kernel/koru_abi.rs.
    CHECK_EQ(p.max_sq_entries, KORU_MAX_SQ_ENTRIES);
    CHECK_EQ(p.max_cq_entries, KORU_MAX_CQ_ENTRIES);
    CHECK_EQ(p.max_slot_size, KORU_MAX_SLOT_SIZE);
    CHECK_EQ(p.max_slot_count, KORU_MAX_SLOT_COUNT);
    CHECK_EQ(p.max_arena_bytes, KORU_MAX_ARENA_BYTES);
    CHECK_EQ(p.max_handles, KORU_MAX_HANDLES);
    CHECK_EQ(p.max_delay_ns, KORU_MAX_DELAY_NS);
}

CASE(setup_rejection_matrix)
{
    // All on one fd: a rejected SETUP must not consume the one shot.
    result<Ring> r = Ring::open();
    REQUIRE(r.ok());
    Ring ring             = std::move(r).take();
    result<koru_params> c = ring.get_params();
    REQUIRE(c.ok());
    koru_params caps = c.value();

    struct Case {
        koru_params p;
        int want;
        const char *what;
    };
    std::vector<Case> cases;
    koru_params p;

    p       = good_request();
    p.magic = 0xdeadbeef;
    cases.push_back({ p, EPROTO, "bad magic is EPROTO, not EINVAL" });
    p             = good_request();
    p.abi_version = 999;
    cases.push_back({ p, EPROTO, "bad abi_version" });
    p       = good_request();
    p.flags = 1;
    cases.push_back({ p, EINVAL, "an unknown SETUP flag bit" });
    p             = good_request();
    p.reserved[1] = 1;
    cases.push_back({ p, EINVAL, "a non-zero reserved word" });
    p            = good_request();
    p.sq_entries = 0;
    cases.push_back({ p, EINVAL, "sq_entries 0" });
    p            = good_request();
    p.sq_entries = caps.max_sq_entries + 1;
    cases.push_back({ p, EINVAL, "sq_entries past the cap" });
    p            = good_request();
    p.cq_entries = caps.max_cq_entries + 1;
    cases.push_back({ p, EINVAL, "cq_entries past the cap" });
    p            = good_request();
    p.cq_entries = 32;
    cases.push_back({ p, EINVAL, "cq_entries below sq_entries" });
    p           = good_request();
    p.slot_size = 0;
    cases.push_back({ p, EINVAL, "slot_size 0" });
    p           = good_request();
    p.slot_size = 100;
    cases.push_back({ p, EINVAL, "slot_size 100" });
    p           = good_request();
    p.slot_size = 4097;
    cases.push_back({ p, EINVAL, "slot_size 4097" });
    p           = good_request();
    p.slot_size = caps.max_slot_size + 1;
    cases.push_back({ p, EINVAL, "slot_size past the cap" });
    p            = good_request();
    p.slot_count = 0;
    cases.push_back({ p, EINVAL, "slot_count 0" });
    p            = good_request();
    p.slot_count = caps.max_slot_count + 1;
    cases.push_back({ p, EINVAL, "slot_count past the cap" });
    p            = good_request();
    p.slot_size  = caps.max_slot_size;
    p.slot_count = caps.max_slot_count;
    cases.push_back({ p, EINVAL, "an arena past the cap is rejected, not clamped" });
    p              = good_request();
    p.handle_count = caps.max_handles + 1;
    cases.push_back({ p, EINVAL, "handle_count past the cap" });

    for (Case &k : cases)
        expect_errno(ring.setup_raw(k.p), k.want, k.what);

    // The one shot survived all sixteen.
    p = good_request();
    REQUIRE(ring.setup_raw(p).ok());
    CHECK_EQ(p.sq_entries, 64); // sq and cq entries round-trip
    CHECK_EQ(p.cq_entries, 128);
    CHECK_EQ(p.slot_size, 4096); // slot size and count round-trip
    CHECK_EQ(p.slot_count, 32);
    // arena_size is slot_size * slot_count.
    CHECK_EQ(p.arena_size, uint64_t(4096) * 32);
    CHECK_EQ(p.configured, 1);
    // handle_count 0 became the default the mirror names.
    CHECK_EQ(p.handle_count, KORU_DEFAULT_HANDLES);

    result<koru_params> q = ring.get_params();
    REQUIRE(q.ok());
    // GET_PARAMS returns what SETUP returned.
    CHECK_EQ(memcmp(&q.value(), &p, sizeof(p)), 0);

    koru_params again = good_request();
    expect_errno(ring.setup_raw(again), EBUSY, "a second SETUP");
}

CASE(setup_zero_fields_take_their_defaults)
{
    {
        result<Ring> r = Ring::open();
        REQUIRE(r.ok());
        Ring ring     = std::move(r).take();
        koru_params p = good_request();
        p.cq_entries  = 0;
        REQUIRE(ring.setup_raw(p).ok());
        // cq_entries 0 becomes sq_entries.
        CHECK_EQ(p.cq_entries, p.sq_entries);
    }
    {
        result<Ring> r = Ring::open();
        REQUIRE(r.ok());
        Ring ring      = std::move(r).take();
        koru_params p  = good_request();
        p.handle_count = 7;
        REQUIRE(ring.setup_raw(p).ok());
        // An explicit handle_count is echoed back.
        CHECK_EQ(p.handle_count, 7);
    }
}

// ---------------------------------------------------------------------------
// T3 - ioctl dispatch
// ---------------------------------------------------------------------------
//
// ENOTTY means no such command; EPROTO means our command with the wrong struct
// size or direction, which is version skew. Keeping them apart is the point.

CASE(ioctl_dispatch_matrix)
{
    koru::SetupConfig cfg;
    cfg.sq_entries   = 32;
    cfg.cq_entries   = 64;
    cfg.slot_size    = 4096;
    cfg.slot_count   = 8;
    cfg.handle_count = 8;
    result<Ring> rr  = Ring::with_config(cfg);
    REQUIRE(rr.ok());
    Ring r = std::move(rr).take();

    koru_params p = {};
    void *arg     = &p;
    size_t psize = sizeof(koru_params), esize = sizeof(koru_enter);
    unsigned rw = _IOC_READ | _IOC_WRITE;

    expect_errno(r.ioctl_raw(_IOC(_IOC_NONE, 'x', 0x7f, 0), arg), ENOTTY, "an unknown ioctl");
    for (unsigned dir = 0; dir < 4; dir++)
        for (unsigned nr = 0; nr < 5; nr++)
            expect_errno(r.ioctl_raw(_IOC(dir, 'z', nr, psize), arg), ENOTTY,
                         "a foreign type byte");
    for (unsigned nr = 3; nr < 8; nr++)
        expect_errno(r.ioctl_raw(_IOC(rw, KORU_IOC_TYPE, nr, psize), arg), ENOTTY,
                     "our type byte, unknown number");

    expect_errno(r.ioctl_raw(_IOC(rw, KORU_IOC_TYPE, KORU_NR_SETUP, psize + 8), arg), EPROTO,
                 "SETUP with the wrong size");
    expect_errno(r.ioctl_raw(_IOC(_IOC_READ, KORU_IOC_TYPE, KORU_NR_SETUP, psize), arg), EPROTO,
                 "SETUP encoded read-only");
    expect_errno(r.ioctl_raw(_IOC(_IOC_WRITE, KORU_IOC_TYPE, KORU_NR_GET_PARAMS, psize), arg),
                 EPROTO, "GET_PARAMS encoded write-only");
    expect_errno(r.ioctl_raw(_IOC(_IOC_NONE, KORU_IOC_TYPE, KORU_NR_GET_PARAMS, psize), arg),
                 EPROTO, "GET_PARAMS with no direction");
    expect_errno(r.ioctl_raw(_IOC(_IOC_WRITE, KORU_IOC_TYPE, KORU_NR_ENTER, esize), arg), EPROTO,
                 "ENTER encoded write-only");

    void *bogus = reinterpret_cast<void *>(0x10);
    expect_errno(r.ioctl_raw(KORU_IOC_GET_PARAMS, bogus), EFAULT, "GET_PARAMS with a bad pointer");
    expect_errno(r.ioctl_raw(KORU_IOC_ENTER, bogus), EFAULT, "ENTER with a bad pointer");
    expect_errno(r.ioctl_raw(KORU_IOC_SETUP, bogus), EFAULT, "SETUP with a bad pointer");
}

// ---------------------------------------------------------------------------
// T39 - the pool and the vocabulary
// ---------------------------------------------------------------------------
//
// The buffer pool is advisory — the kernel's bitmap is what enforces slot
// exclusivity — so these are about the C++ half alone: that a slot goes back
// when it is dropped, and that a move leaves nothing behind that would give it
// back twice.

CASE(pool_hands_out_every_slot_and_takes_them_back)
{
    Mapped m   = Mapped::shared();
    uint32_t n = m.slot_count();
    koru::BufPool pool(std::move(m.arena));
    CHECK_EQ(pool.free_count(), n);

    {
        std::vector<koru::BufSlot> held;
        for (uint32_t i = 0; i < n; i++) {
            koru::BufSlot s = pool.acquire();
            if (!s.held()) {
                FAILF("slot %u could not be acquired", i);
                return;
            }
            held.push_back(std::move(s));
        }
        CHECK_EQ(pool.free_count(), 0);
        // An empty pool hands back a slot that is not held, never a bad index.
        koru::BufSlot none = pool.acquire();
        CHECK(!none.held());
        CHECK_EQ(none.size(), 0);

        // Every index is distinct, and its bytes are that slot of the arena.
        std::vector<bool> seen(n, false);
        for (koru::BufSlot &s : held) {
            if (s.index() >= n || seen[s.index()]) {
                FAILF("index %u is out of range or handed out twice", s.index());
                return;
            }
            seen[s.index()] = true;
            CHECK_EQ(s.size(), pool.slot_size());
            CHECK(s.bytes().data() == pool.arena().slot(s.index()).data());
        }
        held[3].release();
        CHECK_EQ(pool.free_count(), 1); // released early
    }
    // And the rest went back with their destructors.
    CHECK_EQ(pool.free_count(), n);
}

CASE(pool_a_moved_slot_leaves_the_source_empty)
{
    Mapped m = Mapped::shared();
    koru::BufPool pool(std::move(m.arena));
    uint32_t n = uint32_t(pool.free_count());

    koru::BufSlot a = pool.acquire();
    REQUIRE(a.held());
    uint32_t index = a.index();
    {
        koru::BufSlot b = std::move(a);
        CHECK(b.held());
        CHECK(!a.held()); // the source gave up its claim
        CHECK_EQ(b.index(), index);
        CHECK_EQ(pool.free_count(), n - 1);
    }
    // One move, one release: a source that still held it would give it back
    // twice and the pool would hand out one index to two owners.
    CHECK_EQ(pool.free_count(), n);
    koru::BufSlot c = pool.acquire();
    koru::BufSlot d = pool.acquire();
    CHECK(c.index() != d.index());
}

CASE(result_carries_a_value_or_an_error_never_both)
{
    result<int> ok(7);
    CHECK(ok.ok());
    CHECK(bool(ok));
    CHECK_EQ(ok.value(), 7);
    CHECK_EQ(ok.value_or(9), 7);

    result<int> bad(Error::from_errno(Errno(ENOENT)));
    CHECK(!bad.ok());
    CHECK(!bool(bad));
    CHECK_EQ(bad.error().raw().value, ENOENT);
    CHECK(bad.error().is(Kind::NotFound));
    CHECK_EQ(bad.value_or(9), 9);

    // A move-only value goes in and comes out again: this is what `Ring` and
    // `Arena` need of it, and a result that copied would not compile.
    result<Ring> r = Ring::open();
    REQUIRE(r.ok());
    Ring ring = std::move(r).take();
    CHECK(ring.is_open());

    result<void> v;
    CHECK(v.ok());
    result<void> e(Error::closed());
    CHECK(!e.ok());
    CHECK(e.error().is(Kind::Closed));
    CHECK_EQ(e.error().raw().value, 0); // synthesised, with no errno preimage
    CHECK_STR(e.error().message(), "closed");
}
