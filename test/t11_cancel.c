// SPDX-License-Identifier: GPL-2.0
//
// T11 done test: CANCEL. The headline case is that cancelling a pending
// DELAY_NS returns at once instead of waiting out the delay.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "koru_test.h"

#define SLOT_SIZE  4096u
#define SLOT_COUNT 8u
#define ARENA      ((uint64_t)SLOT_SIZE * SLOT_COUNT)
#define HANDLES    8u
#define RACE_ITERS 3000

#define PATFILE "/tmp/koru-t11-pattern"
#define PATSIZE 8192u

static uint8_t *arena;

static const struct koru_cqe *find(const struct koru_cqe *cq, unsigned n, uint64_t ud)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (cq[i].user_data == ud)
            return &cq[i];
    return NULL;
}

/* The point of the whole task: a cancelled delay must not be waited out. */
static void headline_test(int fd)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[2];
    unsigned completed = 0;
    const struct koru_cqe *t, *c;
    uint64_t t0;
    int ret;

    /* cq_space 0: reap nothing here, so the cancel call below counts both. */
    sqe_delay(&sq[0], 0x10, 2000 * MS);
    ret = submit(fd, sq, 1, cq, 0, 0, &completed);
    check(ret == 1 && completed == 0, "a 2 s DELAY_NS is queued");

    t0 = now_ms();
    sqe_cancel(&sq[0], 0x10, 0x11);
    ret              = submit(fd, sq, 1, cq, 2, 2, &completed);
    uint64_t elapsed = now_ms() - t0;

    check(ret == 1 && completed == 2, "CANCEL and its target both complete");
    t = find(cq, completed, 0x10);
    c = find(cq, completed, 0x11);
    if (t && c) {
        check_res(c->res, 0, "  the canceller gets 0");
        check_res(t->res, -ECANCELED, "  the target gets ECANCELED");
    } else {
        check(0, "  both CQEs carry their own user_data");
    }
    /* Without a real dequeue this waits the full two seconds. */
    check(elapsed < 500, "  it returned at once, not after the delay");
    if (elapsed >= 500)
        printf("    elapsed %llu ms\n", (unsigned long long)elapsed);
}

static void reject_tests(int fd)
{
    struct koru_sqe s;

    sqe_cancel(&s, 0xdeadbeef, 0x20);
    check_res(run_one(fd, &s), -ENOENT, "CANCEL of an unknown user_data is ENOENT");

    sqe_nop(&s, 0x21);
    check_res(run_one(fd, &s), 0, "a NOP completes");
    sqe_cancel(&s, 0x21, 0x22);
    check_res(run_one(fd, &s), -ENOENT, "  CANCEL of a completed op is ENOENT");

    sqe_cancel(&s, 0x10, 0x23);
    check_res(run_one(fd, &s), -ENOENT, "a second CANCEL of a cancelled op is ENOENT");

    sqe_cancel(&s, 1, 0x24);
    s.len = 1;
    check_res(run_one(fd, &s), -EINVAL, "CANCEL with a non-zero len is EINVAL");
    sqe_cancel(&s, 1, 0x25);
    s.slot = 1;
    check_res(run_one(fd, &s), -EINVAL, "  non-zero slot is EINVAL");
    sqe_cancel(&s, 1, 0x26);
    s.handle = 1;
    check_res(run_one(fd, &s), -EINVAL, "  non-zero handle is EINVAL");
}

/* Asking for more completions than can ever arrive must return a short count,
 * not sleep. Found by T11's leak loop, where a cancel left one CQE queued and
 * nothing in flight. */
static void unreachable_wait_test(int fd)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    unsigned completed = 0;
    uint64_t t0        = now_ms();
    int ret;

    sqe_nop(&sq[0], 0x60);
    ret = submit(fd, sq, 1, cq, 4, 3, &completed);
    check(ret == 1 && completed == 1, "min_complete past what can arrive returns short");
    check(now_ms() - t0 < 500, "  and returns at once rather than sleeping");
}

/* C1 must hold however the race falls out. `delay_ns` 0 queues the op
 * immediately, so a worker can beat the cancel and the EALREADY/ENOENT arms get
 * exercised; a real delay leaves the timer armed and the cancel always wins. */
static void race_test(int fd, uint64_t delay_ns, const char *what)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[4];
    unsigned completed;
    int i, bad_cancel = 0, bad_target = 0, bad_count = 0;
    int n_ok = 0, n_already = 0, n_enoent = 0;

    for (i = 0; i < RACE_ITERS; i++) {
        const struct koru_cqe *t, *c;
        int ret;

        sqe_delay(&sq[0], 0x30, delay_ns);
        if (submit(fd, sq, 1, cq, 0, 0, &completed) != 1)
            break;

        sqe_cancel(&sq[0], 0x30, 0x31);
        ret = submit(fd, sq, 1, cq, 4, 2, &completed);

        /* Every consumed SQE produces exactly one CQE. */
        if (ret != 1 || completed != 2) {
            bad_count = 1;
            printf("    iter %d: ret %d completed %u\n", i, ret, completed);
            break;
        }
        t = find(cq, completed, 0x30);
        c = find(cq, completed, 0x31);
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
            printf("    iter %d: canceller res %lld\n", i, (long long)c->res);
            break;
        }
        if (t->res != 0 && t->res != -ECANCELED) {
            bad_target = 1;
            printf("    iter %d: target res %lld\n", i, (long long)t->res);
            break;
        }
    }

    printf("%s\n", what);
    check(!bad_count, "  every SQE yields exactly one CQE");
    check(!bad_cancel, "  the canceller is only ever 0, EALREADY or ENOENT");
    check(!bad_target, "  the target is only ever 0 or ECANCELED");
    printf("    cancelled %d, already running %d, already done %d\n", n_ok, n_already, n_enoent);
}

/* DELAY_NS does no work, so the window where it is executing is vanishingly
 * small. CHECKSUM over a whole slot is the longest-running op there is, which is
 * the best chance of observing EALREADY. */
static void already_test(int fd)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    unsigned completed;
    int i, n_ok = 0, n_already = 0, n_enoent = 0, bad = 0;

    memset(arena + 3 * SLOT_SIZE, 0x5a, SLOT_SIZE);
    for (i = 0; i < RACE_ITERS; i++) {
        const struct koru_cqe *c;

        sqe_checksum(&sq[0], 3, 0, SLOT_SIZE, 0x70);
        if (submit(fd, sq, 1, cq, 0, 0, &completed) != 1)
            break;
        sqe_cancel(&sq[0], 0x70, 0x71);
        if (submit(fd, sq, 1, cq, 4, 2, &completed) != 1 || completed != 2) {
            bad = 1;
            break;
        }
        c = find(cq, completed, 0x71);
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
    check(!bad, "3000 cancel races against a running CHECKSUM hold C1");
    printf("    cancelled %d, already running %d, already done %d\n", n_ok, n_already, n_enoent);
}

/* A cancelled op must release the slot it held. */
static void slot_test(int fd)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    unsigned completed = 0;
    int64_t r;

    memset(arena + 2 * SLOT_SIZE, 0xa5, SLOT_SIZE);
    sqe_checksum(&sq[0], 2, 0, SLOT_SIZE, 0x40);
    if (submit(fd, sq, 1, cq, 0, 0, &completed) != 1)
        return;

    sqe_cancel(&sq[0], 0x40, 0x41);
    submit(fd, sq, 1, cq, 4, 2, &completed);
    check(completed == 2, "a CHECKSUM and its CANCEL both complete");

    /* Whatever the cancel returned, slot 2 must be usable again. */
    sqe_checksum(&sq[0], 2, 0, SLOT_SIZE, 0x42);
    r = run_one(fd, &sq[0]);
    check(r >= 0, "  the cancelled op released its slot");
    if (r < 0)
        printf("    res %lld\n", (long long)r);
}

static long file_nr(void)
{
    FILE *f    = fopen("/proc/sys/fs/file-nr", "r");
    long alloc = -1;

    if (!f)
        return -1;
    if (fscanf(f, "%ld", &alloc) != 1)
        alloc = -1;
    fclose(f);
    return alloc;
}

static long file_nr_settled(void)
{
    usleep(100000);
    return file_nr();
}

/* The refcount bug shows up here: a leaked OpWork leaks its ARef<File> too,
 * and kmemleak cannot see it because the registry still references it. */
static void leak_test(int fd)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    unsigned completed;
    long before, after;
    uint32_t n;
    int i, bad = 0;

    n      = put_path(arena, SLOT_SIZE, 0, PATFILE);
    before = file_nr_settled();

    for (i = 0; i < 1000; i++) {
        int64_t h;

        sqe_open(&sq[0], 0, 0, n, KORU_O_RDONLY, 0x50);
        if (submit(fd, sq, 1, cq, 4, 1, &completed) != 1 || completed != 1 || cq[0].res <= 0) {
            bad = 1;
            break;
        }
        h = cq[0].res;

        sqe_read(&sq[0], (uint32_t)h, 1, 0, SLOT_SIZE, 0x51);
        if (submit(fd, sq, 1, cq, 0, 0, &completed) != 1) {
            bad = 1;
            break;
        }
        sqe_cancel(&sq[0], 0x51, 0x52);
        if (submit(fd, sq, 1, cq, 4, 2, &completed) != 1 || completed != 2) {
            bad = 1;
            break;
        }
        sqe_close(&sq[0], (uint32_t)h, 0x53);
        if (run_one(fd, &sq[0]) != 0) {
            bad = 1;
            break;
        }
    }
    check(!bad, "1000 open/READ/cancel/close rounds all complete");

    after = file_nr_settled();
    check(before > 0 && after > 0 && after - before < 64,
          "  no OpWork leaked: allocated struct file is unchanged");
    if (after - before >= 64)
        printf("    file-nr %ld -> %ld\n", before, after);
}

int main(void)
{
    int fd;

    test_begin(300);

    if (make_pattern_file(PATFILE, PATSIZE) != 0)
        return 1;

    fd = open_ring_handles(64, 128, SLOT_SIZE, SLOT_COUNT, HANDLES);
    if (fd < 0)
        return 1;
    arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (arena == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    headline_test(fd);
    reject_tests(fd);
    unreachable_wait_test(fd);
    race_test(fd, 2 * MS, "3000 cancel races against an armed timer:");
    race_test(fd, 0, "3000 cancel races against the worker:");
    already_test(fd);
    slot_test(fd);
    leak_test(fd);

    munmap(arena, ARENA);
    close(fd);
    unlink(PATFILE);
    return test_end();
}
