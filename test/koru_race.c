// SPDX-License-Identifier: MIT
//
// The heavy phase: bulk allocation churn and the race loops. Runs first so one
// kmemleak scan at the end sees all of it; anything allocation-heavy added to
// the tail instead is invisible to that scan.
//
// Why the iteration counts are what they are: doc/Notes.md.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "koru_check.h"

#define DEV_ITERS    2000
#define RING_ITERS   300
#define OPEN_ITERS   500
#define READ_ITERS   400
#define CANCEL_ITERS 300
/* Longer: this is the only loop that reaches the -EALREADY window. */
#define ALREADY_ITERS 1000
#define LEAK_ITERS    200

void sec_devchurn(void)
{
    int i, bad = 0;

    for (i = 0; i < DEV_ITERS; i++) {
        int fd = open(KORU_DEV, O_RDWR);

        if (fd < 0) {
            bad = 1;
            break;
        }
        close(fd);
    }
    check(!bad, "2000 open/close cycles on /dev/koru");
}

/* A third abandoned with work still queued. The largest churn in the run. */
void sec_ringchurn(void)
{
    struct koru_ring m;
    struct koru_sqe sq[4];
    struct koru_cqe cq[4];
    unsigned completed = 0;
    int i, bad = 0, in_flight = 0;

    for (i = 0; i < RING_ITERS && !bad; i++) {
        uint32_t slots = 4 + (unsigned)i % 5; /* rotate the geometry */

        if (ring_open(&m, 32, 64, 4096, slots, 8) != 0 || ring_map(&m) != 0) {
            bad = 1;
            ring_close(&m);
            break;
        }
        switch (i % 3) {
        case 0:
            sqe_nop(&sq[0], 0x10);
            sqe_checksum(&sq[1], 0, 0, 4096, 0x11);
            if (submit(m.fd, sq, 2, cq, 2, 2, &completed) != 2 || completed != 2)
                bad = 1;
            break;
        case 1: {
            /* A queued READ: release() must drain the handle table too. */
            int64_t h = r_open(&m, 0, PATFILE, KORU_O_RDONLY);

            if (h > 0) {
                sqe_read(&sq[0], (uint32_t)h, 1, 0, 4096, 0x12);
                submit(m.fd, sq, 1, cq, 0, 0, &completed);
                in_flight++;
            } else {
                bad = 1;
            }
            break;
        }
        default:
            /* Long enough that close() must cancel, not wait it out. */
            sqe_delay(&sq[0], 0x13, 500 * MS);
            if (submit(m.fd, sq, 1, cq, 0, 0, &completed) != 1)
                bad = 1;
            in_flight++;
            break;
        }
        ring_close(&m);
    }
    check(!bad, "300 ring lifecycles, rotating geometry");
    check_ge(in_flight, 150, "  most torn down with work still queued");
}

/* The 2 s delay keeps the ring's Arc alive across the close, so release() has
 * to drain the handle table explicitly. */
static void drain_test(void)
{
    struct koru_ring m;
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    unsigned completed = 0;
    long before, during, after;
    uint32_t n;

    before = file_nr_settled();
    if (ring_open(&m, 32, 64, 8192, 4, 8) != 0 || ring_map(&m) != 0) {
        failures++;
        ring_close(&m);
        return;
    }

    n = put_path(m.arena, m.slot_size, 0, HOSTNAME);
    sqe_delay(&sq[0], 0xb0, 2000 * MS);
    sqe_open(&sq[1], 0, 0, n, KORU_O_RDONLY, 0xb1);
    check(submit(m.fd, sq, 2, cq, 2, 1, &completed) == 2 && completed == 1 && cq[0].res > 0,
          "a handle opened with a 2 s delay still queued");

    during = file_nr();
    check(during >= before + 2, "  the handle and the ring fd are both counted");

    ring_close(&m);
    after = file_nr_settled();
    check(after <= before, "  closing the ring drops the handle at once, not in 2 s");
    if (after > before)
        note("file-nr %ld -> %ld -> %ld", before, during, after);
}

/* kmemleak cannot see a leaked handle: our own table still references it. */
void sec_handles(void)
{
    long before, after;
    int fds_before, fds_after;
    int i, bad = 0;

    fds_before = count_fds();
    before     = file_nr_settled();
    for (i = 0; i < OPEN_ITERS; i++) {
        int64_t h = r_open(&R, 0, HOSTNAME, KORU_O_RDONLY);

        if (h <= 0 || r_close(&R, (uint32_t)h) != 0) {
            bad = 1;
            break;
        }
    }
    check(!bad, "500 OPEN/CLOSE pairs all succeed");
    after     = file_nr_settled();
    fds_after = count_fds();

    check(fds_before >= 0 && fds_after == fds_before, "  /proc/self/fd is unchanged");
    check(before > 0 && after > 0 && after - before < 64, "  no struct file leaked");
    if (after - before >= 64)
        note("file-nr %ld -> %ld", before, after);

    drain_test();
}

/* ------------------------------------------------------------------------- */

/* CLOSE runs inline while the READ is still queued, which is why the work item
 * owns an ARef resolved at submit time. */
static void read_race(void)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    const struct koru_cqe *rd, *cl;
    unsigned completed = 0;
    size_t j;
    int i, bad = 0, close_first = 0, bytes_ok = 1;

    for (i = 0; i < READ_ITERS; i++) {
        int64_t h = r_open(&R, 0, PATFILE, KORU_O_RDONLY);

        if (h <= 0) {
            bad = 1;
            break;
        }
        memset(R.arena + R.slot_size, 0, R.slot_size);
        sqe_read(&sq[0], (uint32_t)h, 1, 0, R.slot_size, 0xb0);
        sqe_close(&sq[1], (uint32_t)h, 0xb1);
        if (submit(R.fd, sq, 2, cq, 2, 2, &completed) != 2 || completed != 2) {
            bad = 1;
            break;
        }
        rd = find_cqe(cq, completed, 0xb0);
        cl = find_cqe(cq, completed, 0xb1);
        if (!rd || !cl || rd->res != PATSIZE || cl->res != 0) {
            bad = 1;
            note("iter %d: read %lld close %lld", i, rd ? (long long)rd->res : -1,
                 cl ? (long long)cl->res : -1);
            break;
        }
        if (cl == &cq[0])
            close_first++;
    }
    for (j = 0; j < PATSIZE; j++)
        if (R.arena[R.slot_size + j] != pattern_byte(j))
            bytes_ok = 0;
    check(!bad, "400 CLOSEs racing an in-flight READ all complete");
    check(bytes_ok, "  and the last one still read the right bytes");
    check_ge(close_first, 350, "  the CLOSE really did land first");
    note("CLOSE completed before the READ in %d of %d", close_first, READ_ITERS);
}

/* An armed timer leaves the op queued and the cancel wins; delay_ns 0 lets a
 * worker beat it. Arms are reported, not gated: doc/Notes.md. */
static void cancel_race(uint64_t delay_ns, int want_ok, int want_enoent, const char *what)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    const struct koru_cqe *t, *c;
    unsigned completed = 0;
    int i, bad_count = 0, bad_cancel = 0, bad_target = 0;
    int n_ok = 0, n_already = 0, n_enoent = 0;

    for (i = 0; i < CANCEL_ITERS; i++) {
        sqe_delay(&sq[0], 0x30, delay_ns);
        if (submit(R.fd, sq, 1, cq, 0, 0, &completed) != 1)
            break;
        sqe_cancel(&sq[0], 0x30, 0x31);
        if (submit(R.fd, sq, 1, cq, 4, 2, &completed) != 1 || completed != 2) {
            bad_count = 1;
            note("iter %d: completed %u", i, completed);
            break;
        }
        t = find_cqe(cq, completed, 0x30);
        c = find_cqe(cq, completed, 0x31);
        if (!t || !c) {
            bad_count = 1;
            break;
        }
        if (c->res == 0)
            n_ok++;
        else if (c->res == -EALREADY)
            n_already++;
        else if (c->res == -ENOENT)
            n_enoent++;
        else {
            bad_cancel = 1;
            note("iter %d: canceller res %lld", i, (long long)c->res);
            break;
        }
        if (t->res != 0 && t->res != -ECANCELED) {
            bad_target = 1;
            note("iter %d: target res %lld", i, (long long)t->res);
            break;
        }
    }
    printf("%s\n", what);
    check(!bad_count, "  every SQE yields exactly one CQE");
    check(!bad_cancel, "  the canceller is only ever 0, EALREADY or ENOENT");
    check(!bad_target, "  the target is only ever 0 or ECANCELED");
    check(n_ok + n_already + n_enoent == CANCEL_ITERS, "  every round is accounted for");
    /* Gate only the arms this shape actually reaches, so a loop that stops
     * reaching them fails instead of passing silently. */
    if (want_ok)
        check_ge(n_ok, want_ok, "  the cancel won often enough");
    if (want_enoent)
        check_ge(n_enoent, want_enoent, "  the worker won often enough");
    note("cancelled %d, already running %d, already done %d", n_ok, n_already, n_enoent);
}

/* A whole-slot CHECKSUM is the longest-running op: the best shot at -EALREADY. */
static void already_race(void)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    const struct koru_cqe *c;
    unsigned completed = 0;
    int i, bad = 0, n_ok = 0, n_already = 0, n_enoent = 0;

    memset(R.arena + 3 * (size_t)R.slot_size, 0x5a, R.slot_size);
    for (i = 0; i < ALREADY_ITERS; i++) {
        sqe_checksum(&sq[0], 3, 0, R.slot_size, 0x70);
        if (submit(R.fd, sq, 1, cq, 0, 0, &completed) != 1)
            break;
        sqe_cancel(&sq[0], 0x70, 0x71);
        if (submit(R.fd, sq, 1, cq, 4, 2, &completed) != 1 || completed != 2) {
            bad = 1;
            break;
        }
        c = find_cqe(cq, completed, 0x71);
        if (!c) {
            bad = 1;
            break;
        }
        if (c->res == 0)
            n_ok++;
        else if (c->res == -EALREADY)
            n_already++;
        else if (c->res == -ENOENT)
            n_enoent++;
        else {
            bad = 1;
            break;
        }
    }
    check(!bad, "1000 cancel races against a running CHECKSUM hold C1");
    check_ge(n_ok, 150, "  the cancel won often enough");
    check_ge(n_enoent, 20, "  and the CHECKSUM won often enough");
    note("cancelled %d, already running %d, already done %d", n_ok, n_already, n_enoent);
}

/* Drop the cancel reference on a false return and it is a use-after-free; skip
 * it on a true return and the op, its file and a module reference all leak. */
static void cancel_leak(void)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    unsigned completed = 0;
    long before, after;
    uint32_t n;
    int i, bad = 0;

    n      = put_path(R.arena, R.slot_size, 0, PATFILE);
    before = file_nr_settled();

    for (i = 0; i < LEAK_ITERS; i++) {
        int64_t h;

        sqe_open(&sq[0], 0, 0, n, KORU_O_RDONLY, 0x50);
        if (submit(R.fd, sq, 1, cq, 4, 1, &completed) != 1 || completed != 1 || cq[0].res <= 0) {
            bad = 1;
            break;
        }
        h = cq[0].res;

        sqe_read(&sq[0], (uint32_t)h, 1, 0, R.slot_size, 0x51);
        if (submit(R.fd, sq, 1, cq, 0, 0, &completed) != 1) {
            bad = 1;
            break;
        }
        sqe_cancel(&sq[0], 0x51, 0x52);
        if (submit(R.fd, sq, 1, cq, 4, 2, &completed) != 1 || completed != 2) {
            bad = 1;
            break;
        }
        sqe_close(&sq[0], (uint32_t)h, 0x53);
        if (run_one(R.fd, &sq[0]) != 0) {
            bad = 1;
            break;
        }
    }
    check(!bad, "200 open/READ/cancel/close rounds all complete");

    after = file_nr_settled();
    check(before > 0 && after > 0 && after - before < 64, "  no OpWork leaked its file reference");
    if (after - before >= 64)
        note("file-nr %ld -> %ld", before, after);
}

void sec_races(void)
{
    read_race();
    /* An armed timer: the cancel always wins. delay_ns 0: the worker usually
     * does. The thresholds are measured values with a wide margin. */
    cancel_race(2 * MS, 250, 0, "cancel races against an armed timer:");
    cancel_race(0, 0, 50, "cancel races against the worker:");
    already_race();
    cancel_leak();
}
