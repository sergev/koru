// SPDX-License-Identifier: GPL-2.0
//
// T12 done test: hostile userspace. Random SQEs and ENTER parameters from eight
// threads on one ring, plus faulting buffers, malformed ioctls, munmap under
// in-flight work and kill -9.
//
// Usage: t12_fuzz [seconds] [seed]. The seed is printed first; passing it back
// replays the same sequence.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "koru_test.h"

#define SQ_ENTRIES 64u
#define CQ_ENTRIES 128u
#define SLOT_SIZE  4096u
#define SLOT_COUNT 24u
#define ARENA      ((uint64_t)SLOT_SIZE * SLOT_COUNT)
#define HANDLES    512u
#define NWORKERS   8
#define BATCH      8u
#define PATFILE    "/tmp/koru-t12-pattern"
#define PATSIZE    8192u

/* Totals for the C1 check. Every ENTER on the shared ring is accounted, from
 * whichever thread issued it: a completion this program consumes without
 * counting would look exactly like a completion the kernel lost. */
static atomic_ullong total_submitted, total_reaped, total_ops;
static atomic_int stop;
static int fd = -1;
static uint8_t *arena;
static uint64_t max_delay_ns; /* the DELAY_NS cap, learned at startup */

/* ------------------------------------------------------------------------- */

/* xorshift64*, per-thread so threads never share PRNG state. */
static uint64_t rnd(uint64_t *s)
{
    uint64_t x = *s;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545f4914f6cdd1dull;
}

static uint32_t rnd_below(uint64_t *s, uint32_t n)
{
    return n ? (uint32_t)(rnd(s) % n) : 0;
}

static int one_in(uint64_t *s, uint32_t n)
{
    return rnd_below(s, n) == 0;
}

/* Handles harvested from OPEN completions. Shared and racy on purpose: a stale
 * or already-closed handle is exactly the input we want. Without this, READ and
 * CLOSE only ever see invented handles and stop at EBADF, so the whole read
 * path goes untested. */
#define POOL 1024
static atomic_uint handle_pool[POOL];
static atomic_uint pool_next;

/* Per-opcode coverage: how many completed at all, and how many succeeded.
 * A path with zero successes is a path the fuzz never really reached. */
static atomic_ullong op_total[8], op_ok[8];
static atomic_ullong open_einval, open_ebusy, open_emfile, open_other;

/* READ borrows a handle; CLOSE consumes it, so opens and closes stay balanced.
 * Without that the table fills once and every later OPEN is EMFILE. */
static uint32_t pool_pick(uint64_t *s)
{
    return atomic_load(&handle_pool[rnd_below(s, POOL)]);
}

static uint32_t pool_take(uint64_t *s)
{
    return atomic_exchange(&handle_pool[rnd_below(s, POOL)], 0);
}

/* ------------------------------------------------------------------------- */

static void fail(const char *what)
{
    printf("%-58s FAIL\n", what);
    failures++;
    atomic_store(&stop, 1);
}

/* Shape invariants that hold for every CQE the kernel can build: there is only
 * one Cqe constructor, so any deviation is a real bug. */
static void check_cqe(const struct koru_cqe *c)
{
    if (c->flags != 0 || c->rsvd0 != 0 || c->extra != 0) {
        printf("    cqe shape: flags %u rsvd0 %u extra %llu\n", c->flags, c->rsvd0,
               (unsigned long long)c->extra);
        fail("every CQE has zero flags, rsvd0 and extra");
    }
}

/* `res` must be in the opcode's allowed set. OPEN is deliberately open-ended:
 * its errno is whatever filp_open returns, and pinning that down would make the
 * fuzzer fail on filesystem behaviour rather than on koru. */
static int res_allowed(uint8_t opcode, int64_t res)
{
    switch (opcode) {
    case KORU_OP_NOP:
        return res == 0 || res == -EINVAL;
    case KORU_OP_DELAY_NS:
        return res == 0 || res == -EINVAL || res == -ENOMEM || res == -EAGAIN || res == -ECANCELED;
    case KORU_OP_OPEN:
        /* A handle, or some errno. Only a non-negative non-handle is wrong. */
        return res < 0 || res >= 0x10000;
    case KORU_OP_READ:
        return (res >= 0 && res <= SLOT_SIZE) || res == -EINVAL || res == -EBADF || res == -EBUSY ||
               res == -ENOMEM || res == -EAGAIN || res == -ECANCELED || res == -EIO ||
               res == -EINTR;
    case KORU_OP_CLOSE:
        return res == 0 || res == -EINVAL || res == -EBADF;
    case KORU_OP_CANCEL:
        return res == 0 || res == -EINVAL || res == -ENOENT || res == -EALREADY;
    case KORU_OP_CHECKSUM:
        return res >= 0 || res == -EINVAL || res == -EBUSY || res == -ENOMEM || res == -EAGAIN ||
               res == -ECANCELED;
    default:
        return res == -EINVAL;
    }
}

/* Every ENTER on the shared ring must be accounted, or the C1 sum is wrong.
 * On failure nothing was consumed and nothing reaped: submit's error path
 * returns before the reap. EINTR is the exception that still writes both. */
static void account(int ret, const struct koru_enter *e)
{
    if (ret >= 0) {
        atomic_fetch_add(&total_submitted, (unsigned)ret);
        atomic_fetch_add(&total_reaped, e->completed);
        atomic_fetch_add(&total_ops, 1);
    } else if (errno == EINTR) {
        atomic_fetch_add(&total_submitted, e->submitted);
        atomic_fetch_add(&total_reaped, e->completed);
    }
}

/* ------------------------------------------------------------------------- */

/* CQEs come back unordered and from other threads' submissions, so the opcode
 * travels in the echoed user_data: high byte the opcode, low byte a tag. */
#define UD(opcode, tag) (((uint64_t)(opcode) << 8) | ((tag) & 0xff))
#define UD_OPCODE(ud)   ((uint8_t)(((ud) >> 8) & 0xff))

/* A plausible op: right shape, random-but-live field values.
 *
 * `path_slot` is private to the calling thread. A path has to survive from the
 * write until the kernel reads it, and put_path zeroes the whole slot, so a
 * shared slot means one thread's memset lands inside another's OPEN and almost
 * every OPEN fails on an embedded NUL. Every other op takes a random slot,
 * which is what exercises the exclusivity bitmap. */
static void gen_valid(uint64_t *s, struct koru_sqe *q, uint64_t ud, uint32_t path_slot)
{
    uint32_t slot = rnd_below(s, SLOT_COUNT);

    switch (rnd_below(s, 9)) {
    case 7: /* CLOSE is weighted: opens must not outrun closes. */
    case 8:
    case 4:
        sqe_close(q, one_in(s, 4) ? (uint32_t)rnd(s) : pool_take(s), ud);
        break;
    case 0:
        sqe_nop(q, ud);
        break;
    case 1:
        sqe_delay(q, ud, rnd_below(s, 3) * MS);
        break;
    case 2: {
        const char *path = one_in(s, 2) ? PATFILE : "/etc/hostname";

        sqe_open(q, path_slot, 0, put_path(arena, SLOT_SIZE, path_slot, path), KORU_O_RDONLY, ud);
        break;
    }
    case 3:
        sqe_read(q, one_in(s, 4) ? (uint32_t)rnd(s) : pool_pick(s), slot, rnd_below(s, PATSIZE),
                 1 + rnd_below(s, SLOT_SIZE), ud);
        break;
    case 5:
        /* Aim at a live delay often enough to get real cancellations. */
        sqe_cancel(q, UD(KORU_OP_DELAY_NS, rnd(s)), ud);
        break;
    default:
        sqe_checksum(q, slot, 0, rnd_below(s, SLOT_SIZE + 1), ud);
        break;
    }
}

/* Adversarial: every field the opcode does not read, every boundary, and
 * opcodes that do not exist. */
static void gen_hostile(uint64_t *s, struct koru_sqe *q, uint64_t ud, uint32_t path_slot)
{
    gen_valid(s, q, ud, path_slot);

    switch (rnd_below(s, 12)) {
    case 0:
        q->opcode = (uint8_t)rnd(s); /* often 7..255 */
        break;
    case 1:
        q->rsvd0 = (uint16_t)rnd(s);
        break;
    case 2:
        q->flags = (uint8_t)(1 + rnd_below(s, 255));
        break;
    case 3:
        q->off = UINT64_MAX; /* check_range's checked_add */
        break;
    case 4:
        q->len = SLOT_SIZE + rnd_below(s, 8); /* at and past the slot */
        break;
    case 5:
        q->len = 4095 + rnd_below(s, 3); /* the PATH_MAX boundary */
        break;
    case 6:
        /* Just past the count, past the bitmap's first 64-bit word, and
         * absurd. The word boundary matters: with a small slot_count the
         * bitmap has spare bits, so slot_count+1 alone stays in bounds and
         * would hide a missing check. */
        switch (rnd_below(s, 3)) {
        case 0:
            q->slot = SLOT_COUNT + rnd_below(s, 4);
            break;
        case 1:
            q->slot = 64 + rnd_below(s, 4096);
            break;
        default:
            q->slot = UINT32_MAX - rnd_below(s, 4);
            break;
        }
        break;
    case 7:
        q->handle = (uint32_t)rnd(s); /* zero generation, stale, out of range */
        break;
    case 8:
        q->opcode = KORU_OP_OPEN;
        q->handle = 3 | (rnd(s) & 0xf0); /* accmode 3, unknown flag bits */
        break;
    case 9:
        q->opcode = KORU_OP_DELAY_NS;
        /* Strictly over the cap, so always rejected. A value at or under it is
         * accepted and then sits in flight for an hour, pinning a CQ
         * reservation; a few thousand of those brick the ring for the rest of
         * the run. The accepted side of the boundary is covered once, in the
         * deterministic phase, where it is cancelled immediately. */
        q->off = max_delay_ns + 1 + rnd_below(s, 4);
        break;
    case 10:
        memset(q, (int)rnd(s), sizeof(*q)); /* pure garbage */
        break;
    default:
        q->off = rnd(s);
        break;
    }
}

/* -------------------------------------------------------------------------
 * Deterministic rules, checked once before the random phase.
 *
 * A "res is in the allowed set" oracle cannot see a check that was *removed*:
 * the op simply succeeds, and success is in the set. The extensibility rules
 * are the ones that matter most and the ones randomness is worst at, so assert
 * them directly.
 * ------------------------------------------------------------------------- */

static unsigned long long drain(void);

static void rules_phase(void)
{
    static const uint8_t ops[] = { KORU_OP_NOP,
                                   KORU_OP_DELAY_NS,
                                   KORU_OP_OPEN,
                                   KORU_OP_READ,
                                   KORU_OP_CLOSE,
                                   KORU_OP_CANCEL,
                                   KORU_OP_CHECKSUM,
                                   7,
                                   200 };
    struct koru_sqe q;
    struct koru_enter e;
    struct koru_params p;
    uint64_t seed = 1;
    unsigned i;
    int bad_rsvd = 0, bad_flags = 0;

    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        gen_valid(&seed, &q, 0, 0);
        q.opcode = ops[i];
        q.rsvd0  = 1;
        if (run_one(fd, &q) != -EINVAL)
            bad_rsvd = 1;

        gen_valid(&seed, &q, 0, 0);
        q.opcode = ops[i];
        q.flags  = 1;
        if (run_one(fd, &q) != -EINVAL)
            bad_flags = 1;
    }
    check(!bad_rsvd, "a non-zero SQE rsvd0 is EINVAL for every opcode");
    check(!bad_flags, "an unknown SQE flag bit is EINVAL for every opcode");

    /* ENTER's own reserved-must-be-zero rules. */
    memset(&e, 0, sizeof(e));
    e.flags = 1;
    check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EINVAL, "an unknown ENTER flag is EINVAL");
    memset(&e, 0, sizeof(e));
    e.reserved[1] = 1;
    check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EINVAL, "a non-zero ENTER reserved is EINVAL");

    /* cq_space past the queue depth. */
    memset(&e, 0, sizeof(e));
    e.cq_space = CQ_ENTRIES + 1;
    check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EINVAL, "cq_space past cq_entries is EINVAL");

    /* The DELAY_NS cap. */
    memset(&p, 0, sizeof(p));
    p.magic       = KORU_MAGIC;
    p.abi_version = KORU_ABI_VERSION;
    ioctl(fd, KORU_IOC_GET_PARAMS, &p);
    {
        struct koru_cqe cq[4];
        unsigned completed = 0;
        int64_t canceller  = INT64_MIN;

        /* Accepted, so it cannot be waited for: submit, then cancel it. */
        sqe_delay(&q, 0x7f, p.max_delay_ns);
        check(submit(fd, &q, 1, cq, 0, 0, &completed) == 1,
              "a delay exactly at the cap is accepted");

        /* Both CQEs arrive, target first, so pick out the canceller's. */
        sqe_cancel(&q, 0x7f, 0x7e);
        submit(fd, &q, 1, cq, 4, 2, &completed);
        for (i = 0; i < completed; i++)
            if (cq[i].user_data == 0x7e)
                canceller = cq[i].res;
        check_res(canceller, 0, "  and can be cancelled again");

        drain();
        sqe_delay(&q, 0x7d, p.max_delay_ns + 1);
        check_res(run_one(fd, &q), -EINVAL, "  one nanosecond past the cap is EINVAL");
    }

    /* The ioctl direction bits. */
    {
        unsigned nr_size = ('k' << _IOC_TYPESHIFT) | (0x01 << _IOC_NRSHIFT) |
                           (sizeof(struct koru_params) << _IOC_SIZESHIFT);

        check_errno(ioctl(fd, (_IOC_WRITE << _IOC_DIRSHIFT) | nr_size, &p), EPROTO,
                    "GET_PARAMS with the wrong direction is EPROTO");
        check_errno(ioctl(fd, (_IOC_NONE << _IOC_DIRSHIFT) | nr_size, &p), EPROTO,
                    "  and with no direction at all");
    }

    /* Leave nothing queued: the C1 sums only cover the random phase. */
    drain();
}

/* ------------------------------------------------------------------------- */

struct wctx {
    uint64_t seed;
    uint32_t path_slot;
};

static void *worker(void *arg)
{
    struct wctx *w = arg;
    uint64_t seed  = w->seed;
    struct koru_sqe sq[BATCH];
    struct koru_cqe cq[CQ_ENTRIES];
    struct koru_enter e;
    unsigned i;

    while (!atomic_load(&stop)) {
        unsigned n     = 1 + rnd_below(&seed, BATCH);
        unsigned space = rnd_below(&seed, CQ_ENTRIES + 1);
        int ret;

        for (i = 0; i < n; i++) {
            uint64_t tag = rnd(&seed);

            if (one_in(&seed, 3))
                gen_hostile(&seed, &sq[i], tag, w->path_slot);
            else
                gen_valid(&seed, &sq[i], tag, w->path_slot);
            /* Stamped last: the hostile generator may have changed the opcode. */
            sq[i].user_data = UD(sq[i].opcode, tag);
        }

        memset(&e, 0, sizeof(e));
        e.sq_addr      = (uint64_t)(uintptr_t)sq;
        e.cq_addr      = (uint64_t)(uintptr_t)cq;
        e.to_submit    = n;
        e.cq_space     = space;
        e.min_complete = rnd_below(&seed, space + 1);
        /* Always bounded when we actually wait: an unbounded wait that is not
         * satisfied stalls this thread for the rest of the run. */
        e.timeout_ns = (rnd_below(&seed, 5) + 1) * MS;
        /* Occasionally poison the fields that must be zero. */
        if (one_in(&seed, 64))
            e.flags = (uint32_t)rnd(&seed);
        if (one_in(&seed, 64))
            e.reserved[rnd_below(&seed, 2)] = rnd(&seed);

        ret = ioctl(fd, KORU_IOC_ENTER, &e);
        account(ret, &e);
        if (ret < 0) {
            if (errno != EINVAL && errno != EFAULT && errno != EINTR) {
                printf("    ENTER errno %d (%s)\n", errno, strerror(errno));
                fail("ENTER only ever fails EINVAL, EFAULT or EINTR");
            }
            continue;
        }
        if ((unsigned)ret > n || e.submitted != (unsigned)ret) {
            printf("    ret %d to_submit %u submitted %u\n", ret, n, e.submitted);
            fail("ENTER returns the consumed count and echoes it in submitted");
            continue;
        }
        if (e.completed > space) {
            fail("ENTER never writes more CQEs than cq_space");
            continue;
        }

        for (i = 0; i < e.completed; i++) {
            uint8_t op = UD_OPCODE(cq[i].user_data);

            check_cqe(&cq[i]);
            /* Echoed verbatim: we only ever submit 16-bit user_data. */
            if (cq[i].user_data > 0xffff) {
                printf("    user_data %llu\n", (unsigned long long)cq[i].user_data);
                fail("CQE user_data is echoed verbatim from its SQE");
                continue;
            }
            if (!res_allowed(op, cq[i].res)) {
                printf("    opcode %u res %lld\n", op, (long long)cq[i].res);
                fail("every CQE res is in its opcode's allowed set");
            }
            atomic_fetch_add(&op_total[op & 7], 1);
            if (cq[i].res >= 0)
                atomic_fetch_add(&op_ok[op & 7], 1);
            if (op == KORU_OP_OPEN && cq[i].res < 0) {
                if (cq[i].res == -EINVAL)
                    atomic_fetch_add(&open_einval, 1);
                else if (cq[i].res == -EBUSY)
                    atomic_fetch_add(&open_ebusy, 1);
                else if (cq[i].res == -EMFILE)
                    atomic_fetch_add(&open_emfile, 1);
                else
                    atomic_fetch_add(&open_other, 1);
            }
            if (op == KORU_OP_OPEN && cq[i].res >= 0x10000) {
                unsigned k = atomic_fetch_add(&pool_next, 1) % POOL;

                atomic_store(&handle_pool[k], (uint32_t)cq[i].res);
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */

/* A buffer whose second page is unmapped, so a copy that runs off the end
 * faults. `at` bytes are usable. */
static uint8_t *guard_buf(size_t *usable)
{
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *p =
        mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED)
        return NULL;
    if (mprotect(p + pg, (size_t)pg, PROT_NONE) != 0) {
        munmap(p, (size_t)pg * 2);
        return NULL;
    }
    *usable = (size_t)pg;
    return p;
}

/* Faulting SQ/CQ buffers, malformed ioctls, and rival SETUPs. C1 accounting is
 * off here: a faulting write-back cannot report its counts. */
static void *chaos(void *arg)
{
    uint64_t seed  = *(uint64_t *)arg;
    size_t usable  = 0;
    uint8_t *guard = guard_buf(&usable);
    struct koru_params p;
    struct koru_enter e;

    while (!atomic_load(&stop)) {
        /* 1. An SQ array that runs off the end of a mapping: the fault lands at
         * entry 0, at entry k, or inside an entry for a torn 32-byte read.
         *
         * cq_space is 0 here on purpose. This thread must not reap: a CQE it
         * swallows is one the workers never see, and an OPEN handle swallowed
         * that way is orphaned, which fills the handle table and turns every
         * later OPEN into EMFILE. */
        if (guard) {
            size_t off = usable - rnd_below(&seed, 96);

            memset(&e, 0, sizeof(e));
            e.sq_addr   = (uint64_t)(uintptr_t)(guard + off);
            e.to_submit = 1 + rnd_below(&seed, 8);
            e.cq_space  = 0;
            memset(guard + off, 0, usable - off);
            account(ioctl(fd, KORU_IOC_ENTER, &e), &e);

            /* A CQ array that faults partway: the reap must stop and leave the
             * rest queued, so the workers still see them and C1 holds. */
            memset(&e, 0, sizeof(e));
            e.cq_addr  = (uint64_t)(uintptr_t)(guard + usable - 16);
            e.cq_space = 1 + rnd_below(&seed, 4);
            account(ioctl(fd, KORU_IOC_ENTER, &e), &e);
        }

        /* 2. A wholly bad address for each of the three pointers. */
        memset(&e, 0, sizeof(e));
        e.sq_addr   = 0x10;
        e.cq_addr   = 0x10;
        e.to_submit = 1;
        e.cq_space  = 1; /* a bad cq_addr reaps nothing: the first write faults */
        account(ioctl(fd, KORU_IOC_ENTER, &e), &e);
        ioctl(fd, KORU_IOC_ENTER, (void *)0x10);
        ioctl(fd, KORU_IOC_SETUP, (void *)0x10);
        ioctl(fd, KORU_IOC_GET_PARAMS, (void *)0x10);

        /* 3. Malformed ioctl numbers: every direction, number and size. */
        {
            unsigned dir  = rnd_below(&seed, 4);
            unsigned nr   = rnd_below(&seed, 5);
            unsigned size = rnd_below(&seed, 2) ? rnd_below(&seed, 200) : 104;
            unsigned cmd = (dir << _IOC_DIRSHIFT) | ('k' << _IOC_TYPESHIFT) | (nr << _IOC_NRSHIFT) |
                           (size << _IOC_SIZESHIFT);

            memset(&p, 0, sizeof(p));
            ioctl(fd, cmd, &p);
            /* A foreign type byte must always be ENOTTY. */
            cmd = (dir << _IOC_DIRSHIFT) | ('z' << _IOC_TYPESHIFT) | (nr << _IOC_NRSHIFT) |
                  (size << _IOC_SIZESHIFT);
            if (ioctl(fd, cmd, &p) == 0 || errno != ENOTTY)
                fail("an unknown ioctl type byte is ENOTTY");
        }

        /* 4. A rival SETUP on the configured ring must lose. */
        memset(&p, 0, sizeof(p));
        p.magic       = KORU_MAGIC;
        p.abi_version = KORU_ABI_VERSION;
        p.sq_entries  = SQ_ENTRIES;
        p.cq_entries  = CQ_ENTRIES;
        p.slot_size   = SLOT_SIZE;
        p.slot_count  = SLOT_COUNT;
        if (ioctl(fd, KORU_IOC_SETUP, &p) == 0 || errno != EBUSY)
            fail("a second SETUP on a configured ring is EBUSY");

        /* 5. A second mmap must lose too: the arena is one-shot. */
        if (mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) != MAP_FAILED)
            fail("a second mmap of the arena is refused");

        usleep(1000);
    }
    if (guard)
        munmap(guard, (size_t)sysconf(_SC_PAGESIZE) * 2);
    return NULL;
}

/* ------------------------------------------------------------------------- */

/* Its own ring, its own arena, unmapped while ops are in flight, then either a
 * clean exit or a SIGKILL. This is what drives release() under load. */
static void child_body(uint64_t seed)
{
    struct koru_sqe sq[BATCH];
    struct koru_cqe cq[16];
    struct koru_enter e;
    uint8_t *a;
    unsigned i;
    int cfd = open_dev();

    if (cfd < 0)
        _exit(1);

    /* Random SETUP, sometimes invalid. */
    {
        struct koru_params p;

        memset(&p, 0, sizeof(p));
        p.magic        = KORU_MAGIC;
        p.abi_version  = KORU_ABI_VERSION;
        p.sq_entries   = one_in(&seed, 8) ? rnd_below(&seed, 9000) : SQ_ENTRIES;
        p.cq_entries   = CQ_ENTRIES;
        p.slot_size    = one_in(&seed, 8) ? rnd_below(&seed, 9000) : SLOT_SIZE;
        p.slot_count   = SLOT_COUNT;
        p.handle_count = one_in(&seed, 8) ? rnd_below(&seed, 9000) : HANDLES;
        if (ioctl(cfd, KORU_IOC_SETUP, &p) != 0)
            _exit(0); /* a rejected SETUP is a fine outcome */
    }

    a = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
    if (a == MAP_FAILED)
        _exit(0);
    put_path(a, SLOT_SIZE, 0, PATFILE);

    for (i = 0; i < BATCH; i++)
        gen_valid(&seed, &sq[i], i, 0);
    /* Long delays and checksums, so work is still queued at teardown. */
    sqe_delay(&sq[0], 0x100, (rnd_below(&seed, 500) + 100) * MS);
    sqe_checksum(&sq[1], 0, 0, SLOT_SIZE, 0x101);

    memset(&e, 0, sizeof(e));
    e.sq_addr   = (uint64_t)(uintptr_t)sq;
    e.cq_addr   = (uint64_t)(uintptr_t)cq;
    e.to_submit = BATCH;
    e.cq_space  = 16;
    ioctl(cfd, KORU_IOC_ENTER, &e);

    /* Unmap under the in-flight ops. The kernel owns those pages. */
    if (one_in(&seed, 2))
        munmap(a, ARENA);
    if (one_in(&seed, 3))
        close(cfd);

    /* Either exit here, or sit and wait to be killed. */
    if (one_in(&seed, 2))
        _exit(0);
    for (;;)
        pause();
}

static void *spawner(void *arg)
{
    uint64_t seed = *(uint64_t *)arg;

    while (!atomic_load(&stop)) {
        pid_t pid = fork();

        if (pid == 0)
            child_body(rnd(&seed));
        if (pid < 0) {
            usleep(50000);
            continue;
        }
        usleep(1000 + rnd_below(&seed, 20000));
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    return NULL;
}

/* SIGUSR1 with no SA_RESTART, so a blocked ENTER returns EINTR. */
static void usr1(int sig)
{
    (void)sig;
}

/* ------------------------------------------------------------------------- */

/* Reap until the ring is quiet, so the C1 sums can be compared. */
static unsigned long long drain(void)
{
    struct koru_cqe cq[CQ_ENTRIES];
    struct koru_enter e;
    unsigned long long got = 0;
    int idle               = 0;

    while (idle < 20) {
        memset(&e, 0, sizeof(e));
        e.cq_addr  = (uint64_t)(uintptr_t)cq;
        e.cq_space = CQ_ENTRIES;
        if (ioctl(fd, KORU_IOC_ENTER, &e) < 0)
            break;
        if (e.completed == 0) {
            idle++;
            usleep(20000);
            continue;
        }
        idle = 0;
        for (unsigned i = 0; i < e.completed; i++)
            check_cqe(&cq[i]);
        got += e.completed;
    }
    return got;
}

int main(int argc, char **argv)
{
    unsigned secs = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 600;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : (uint64_t)time(NULL) * 2654435761u;
    pthread_t th[NWORKERS + 2];
    uint64_t seeds[NWORKERS + 2];
    struct wctx wc[NWORKERS];
    struct koru_params p;
    struct sigaction sa;
    uint64_t deadline;
    int i, nth = 0;

    if (seed == 0)
        seed = 1;
    /* Line-buffered, and armed well past the run so a wedge still reports. */
    test_begin(secs + 300);
    printf("t12_fuzz: %u seconds, seed %llu\n", secs, (unsigned long long)seed);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1;
    sigaction(SIGUSR1, &sa, NULL); /* no SA_RESTART: ENTER must give EINTR */

    if (make_pattern_file(PATFILE, PATSIZE) != 0)
        return 1;

    fd = open_ring_handles(SQ_ENTRIES, CQ_ENTRIES, SLOT_SIZE, SLOT_COUNT, HANDLES);
    if (fd < 0)
        return 1;
    arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (arena == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Learn the caps rather than hardcoding them. */
    memset(&p, 0, sizeof(p));
    p.magic       = KORU_MAGIC;
    p.abi_version = KORU_ABI_VERSION;
    if (ioctl(fd, KORU_IOC_GET_PARAMS, &p) != 0) {
        perror("GET_PARAMS");
        return 1;
    }
    check(p.max_delay_ns > 0, "GET_PARAMS reports max_delay_ns");
    /* Straddle the cap: some accepted, some EINVAL. */
    max_delay_ns = p.max_delay_ns;

    rules_phase();

    for (i = 0; i < NWORKERS; i++) {
        wc[i].seed = seed + (uint64_t)i * 0x9e3779b97f4a7c15ull;
        /* One private slot each, taken from the top of the arena. */
        wc[i].path_slot = SLOT_COUNT - 1 - (uint32_t)i;
        pthread_create(&th[nth], NULL, worker, &wc[i]);
        nth++;
    }
    seeds[nth] = seed ^ 0xfeedfaceull;
    pthread_create(&th[nth], NULL, chaos, &seeds[nth]);
    nth++;
    seeds[nth] = seed ^ 0xdeadbeefull;
    pthread_create(&th[nth], NULL, spawner, &seeds[nth]);
    nth++;

    /* Interrupt the workers while they wait, exercising the EINTR path. */
    deadline = now_ms() + (uint64_t)secs * 1000;
    while (now_ms() < deadline && !atomic_load(&stop)) {
        usleep(20000);
        for (i = 0; i < NWORKERS; i++)
            pthread_kill(th[i], SIGUSR1);
    }
    atomic_store(&stop, 1);
    for (i = 0; i < nth; i++)
        pthread_join(th[i], NULL);
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    {
        unsigned long long tail = drain();
        unsigned long long sub  = atomic_load(&total_submitted);
        unsigned long long rea  = atomic_load(&total_reaped) + tail;

        printf("    %llu ENTERs, %llu SQEs consumed, %llu CQEs reaped (%llu at drain)\n",
               (unsigned long long)atomic_load(&total_ops), sub, rea, tail);
        static const char *names[8] = { "NOP",   "DELAY",  "OPEN",  "READ",
                                        "CLOSE", "CANCEL", "CKSUM", "other" };
        int op;

        for (op = 0; op < 8; op++)
            printf("    %-6s %8llu completed, %8llu succeeded\n", names[op],
                   (unsigned long long)atomic_load(&op_total[op]),
                   (unsigned long long)atomic_load(&op_ok[op]));
        printf("    OPEN fails: EINVAL %llu EBUSY %llu EMFILE %llu other %llu\n",
               (unsigned long long)atomic_load(&open_einval),
               (unsigned long long)atomic_load(&open_ebusy),
               (unsigned long long)atomic_load(&open_emfile),
               (unsigned long long)atomic_load(&open_other));
        check(sub > 1000, "the fuzzer actually exercised the ring");
        for (op = 0; op <= KORU_OP_CHECKSUM; op++)
            if (atomic_load(&op_ok[op]) == 0) {
                printf("    opcode %d never once succeeded\n", op);
                fail("every opcode succeeded at least once");
                break;
            }
        /* C1: every consumed SQE produced exactly one CQE. */
        check(sub == rea, "C1 holds: consumed SQEs == reaped CQEs");
        if (sub != rea)
            printf("    consumed %llu, reaped %llu\n", sub, rea);
    }

    munmap(arena, ARENA);
    close(fd);
    unlink(PATFILE);
    return test_end();
}
