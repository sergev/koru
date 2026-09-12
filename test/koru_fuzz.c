// SPDX-License-Identifier: MIT
//
// Hostile userspace: random SQEs and ENTER parameters from eight threads on one
// ring, plus faulting buffers, malformed ioctls, munmap under in-flight work
// and kill -9. Runs in a forked child for a fixed three seconds.
//
// Why three seconds, and what that costs: doc/Notes.md.

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
#include <unistd.h>

#include "koru_check.h"

/* The only path the fuzzer ever opens for writing. */
#define FUZZWRFILE "/tmp/koru-fuzz-write"

#define F_SQ      64u
#define F_CQ      128u
#define F_SLOT    4096u
#define F_SLOTS   24u
#define F_ARENA   ((uint64_t)F_SLOT * F_SLOTS)
#define F_HANDLES 128u
#define NWORKERS  8
#define BATCH     8u
#define SECONDS   3u
#define SUB_FLOOR 5000ull

static atomic_ullong total_submitted, total_reaped, total_ops;
static atomic_int stop;
static int fd = -1;
static uint8_t *arena;
static uint64_t max_delay_ns;
static int adoptable = -1; /* read-only, never 0/1/2: see gen_valid */

/* xorshift64*, per thread so threads never share PRNG state. */
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

/* Handles harvested from OPEN completions. Shared and racy on purpose. */
#define POOL 1024
static atomic_uint handle_pool[POOL];
static atomic_uint pool_next;

static atomic_ullong op_total[16], op_ok[16];
static atomic_ullong open_einval, open_ebusy, open_emfile, open_other;

static uint32_t pool_pick(uint64_t *s)
{
    return atomic_load(&handle_pool[rnd_below(s, POOL)]);
}

/* READ borrows a handle, CLOSE consumes it, so opens and closes stay balanced. */
static uint32_t pool_take(uint64_t *s)
{
    return atomic_exchange(&handle_pool[rnd_below(s, POOL)], 0);
}

static void fail(const char *what)
{
    printf("%-58s FAIL\n", what);
    failures++;
    atomic_store(&stop, 1);
}

/* There is only one Cqe constructor, so any deviation is a real bug. */
static void check_cqe(const struct koru_cqe *c)
{
    if (c->flags != 0 || c->rsvd0 != 0 || c->extra != 0) {
        note("cqe shape: flags %u rsvd0 %u extra %llu", c->flags, c->rsvd0,
             (unsigned long long)c->extra);
        fail("every CQE has zero flags, rsvd0 and extra");
    }
}

/* OPEN is open-ended: its errno is whatever filp_open returns. */
static int res_allowed(uint8_t opcode, int64_t res)
{
    switch (opcode) {
    case KORU_OP_NOP:
        return res == 0 || res == -EINVAL;
    case KORU_OP_DELAY_NS:
        return res == 0 || res == -EINVAL || res == -ENOMEM || res == -EAGAIN || res == -ECANCELED;
    case KORU_OP_OPEN:
        return res < 0 || res >= 0x10000;
    case KORU_OP_READ:
        return (res >= 0 && res <= F_SLOT) || res == -EINVAL || res == -EBADF || res == -EBUSY ||
               res == -ENOMEM || res == -EAGAIN || res == -ECANCELED || res == -EIO ||
               res == -EINTR;
    case KORU_OP_CLOSE:
        return res == 0 || res == -EINVAL || res == -EBADF;
    case KORU_OP_CANCEL:
        return res == 0 || res == -EINVAL || res == -ENOENT || res == -EALREADY;
    case KORU_OP_WRITE:
        return (res >= 0 && res <= F_SLOT) || res == -EINVAL || res == -EBADF || res == -EBUSY ||
               res == -ENOMEM || res == -EAGAIN || res == -ECANCELED || res == -EIO ||
               res == -EINTR || res == -ENOSPC || res == -EFBIG || res == -EDQUOT;
    case KORU_OP_ADOPT_FD:
        return res >= 0x10000 || res == -EINVAL || res == -EBADF || res == -ELOOP ||
               res == -EMFILE;
    case KORU_OP_CHECKSUM:
        return res >= 0 || res == -EINVAL || res == -EBUSY || res == -ENOMEM || res == -EAGAIN ||
               res == -ECANCELED;
    case KORU_OP_POLL_ADD:
        /* A regular file is always ready, so res is a mask; anything else is a
         * rejection. EOPNOTSUPP needs two waitqueues, which none of these have. */
        return (res >= 0 && res <= KORU_POLL_EVENTS_ALL) || res == -EINVAL || res == -EBADF ||
               res == -ENOMEM || res == -ECANCELED;
    default:
        return res == -EINVAL;
    }
}

/* Every ENTER must be accounted, or the C1 sum is wrong. EINTR is the failure
 * that still writes both counts. */
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

/* CQEs come back unordered, so the opcode travels in the echoed user_data. */
#define UD(opcode, tag) (((uint64_t)(opcode) << 8) | ((tag) & 0xff))
#define UD_OPCODE(ud)   ((uint8_t)(((ud) >> 8) & 0xff))

/* `path_slot` is private to the calling thread: put_path zeroes the whole slot,
 * so a shared one means every OPEN fails on an embedded NUL. */
static void gen_valid(uint64_t *s, struct koru_sqe *q, uint64_t ud, uint32_t path_slot)
{
    uint32_t slot = rnd_below(s, F_SLOTS);

    switch (rnd_below(s, 16)) {
    case 0:
        sqe_nop(q, ud);
        break;
    case 1:
        sqe_delay(q, ud, rnd_below(s, 3) * MS);
        break;
    case 2:
    case 3:
    case 4: {
        const char *path;
        uint32_t flags;

        /* WRFILE is the only path ever opened for writing. A writable handle
         * plus a random offset would otherwise corrupt whatever it names.
         * No FIFO here either: the hostile generator can clear O_NONBLOCK,
         * and filp_open on a peerless FIFO would then hang a worker. */
        if (one_in(s, 3)) {
            path  = FUZZWRFILE;
            flags = KORU_O_RDWR;
        } else {
            path  = one_in(s, 2) ? PATFILE : HOSTNAME;
            flags = KORU_O_RDONLY;
        }
        if (one_in(s, 4))
            flags |= KORU_O_NONBLOCK; /* a no-op on a regular file */
        sqe_open(q, path_slot, 0, put_path(arena, F_SLOT, path_slot, path), flags, ud);
        break;
    }
    case 5:
    case 6:
        sqe_read(q, one_in(s, 4) ? (uint32_t)rnd(s) : pool_pick(s), slot, rnd_below(s, PATSIZE),
                 1 + rnd_below(s, F_SLOT), ud);
        break;
    case 7:
    case 8:
        /* A read-only handle here is a fine outcome: EBADF. */
        sqe_write(q, one_in(s, 4) ? (uint32_t)rnd(s) : pool_pick(s), slot, rnd_below(s, PATSIZE),
                  1 + rnd_below(s, F_SLOT), ud);
        break;
    case 9:
    case 10:
    case 11:
        sqe_close(q, one_in(s, 4) ? (uint32_t)rnd(s) : pool_take(s), ud);
        break;
    case 12:
        sqe_cancel(q, UD(KORU_OP_DELAY_NS, rnd(s)), ud);
        break;
    case 13:
        /* Only a read-only scratch fd, the ring itself (ELOOP) and a number
         * that is no fd at all. Never 0, 1 or 2: an adopted stdout plus a
         * random WRITE would shred the check's own output. */
        switch (rnd_below(s, 3)) {
        case 0:
            sqe_adopt(q, adoptable, ud);
            break;
        case 1:
            sqe_adopt(q, fd, ud);
            break;
        default:
            sqe_adopt(q, 1 << 20, ud);
            break;
        }
        break;
    case 14:
        /* Only ever a regular file or a bad handle, so a poll here is always
         * ready or refused and none is left armed holding a reservation. The
         * FIFO stays out for the reason the OPEN arm does. */
        sqe_poll(q, one_in(s, 4) ? (uint32_t)rnd(s) : pool_pick(s),
                 one_in(s, 8) ? (uint32_t)rnd(s) : (1u + rnd_below(s, KORU_POLL_EVENTS_ALL)), ud);
        break;
    default:
        sqe_checksum(q, slot, 0, rnd_below(s, F_SLOT + 1), ud);
        break;
    }
}

/* Every field the opcode does not read, every boundary, and opcodes that do not
 * exist. */
static void gen_hostile(uint64_t *s, struct koru_sqe *q, uint64_t ud, uint32_t path_slot)
{
    gen_valid(s, q, ud, path_slot);

    switch (rnd_below(s, 12)) {
    case 0:
        q->opcode = (uint8_t)rnd(s);
        break;
    case 1:
        q->rsvd0 = (uint16_t)rnd(s);
        break;
    case 2:
        q->flags = (uint8_t)(1 + rnd_below(s, 255));
        break;
    case 3:
        q->off = UINT64_MAX;
        break;
    case 4:
        q->len = F_SLOT + rnd_below(s, 8);
        break;
    case 5:
        q->len = 4095 + rnd_below(s, 3); /* the PATH_MAX boundary */
        break;
    case 6:
        /* Past the count, past the bitmap's first word, and absurd. */
        switch (rnd_below(s, 3)) {
        case 0:
            q->slot = F_SLOTS + rnd_below(s, 4);
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
        q->handle = (uint32_t)rnd(s);
        break;
    case 8:
        q->opcode = KORU_OP_OPEN;
        q->handle = 3 | (rnd(s) & 0xf0); /* accmode 3, unknown flag bits */
        break;
    case 9:
        /* Strictly over the cap: an accepted long delay bricks the ring. */
        q->opcode = KORU_OP_DELAY_NS;
        q->off    = max_delay_ns + 1 + rnd_below(s, 4);
        break;
    case 10:
        memset(q, (int)rnd(s), sizeof(*q));
        break;
    default:
        q->off = rnd(s);
        break;
    }
}

struct wctx {
    uint64_t seed;
    uint32_t path_slot;
};

static void *worker(void *arg)
{
    struct wctx *w = arg;
    uint64_t seed  = w->seed;
    struct koru_sqe sq[BATCH];
    struct koru_cqe cq[F_CQ];
    struct koru_enter e;
    unsigned i;

    while (!atomic_load(&stop)) {
        unsigned n     = 1 + rnd_below(&seed, BATCH);
        unsigned space = rnd_below(&seed, F_CQ + 1);
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
        /* Always bounded: an unsatisfied unbounded wait stalls this thread. */
        e.timeout_ns = (rnd_below(&seed, 5) + 1) * MS;
        if (one_in(&seed, 64))
            e.flags = (uint32_t)rnd(&seed);
        if (one_in(&seed, 64))
            e.reserved[rnd_below(&seed, 2)] = rnd(&seed);

        ret = ioctl(fd, KORU_IOC_ENTER, &e);
        account(ret, &e);
        if (ret < 0) {
            if (errno != EINVAL && errno != EFAULT && errno != EINTR) {
                note("ENTER errno %d (%s)", errno, strerror(errno));
                fail("ENTER only ever fails EINVAL, EFAULT or EINTR");
            }
            continue;
        }
        if ((unsigned)ret > n || e.submitted != (unsigned)ret) {
            note("ret %d to_submit %u submitted %u", ret, n, e.submitted);
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
            if (cq[i].user_data > 0xffff) {
                note("user_data %llu", (unsigned long long)cq[i].user_data);
                fail("CQE user_data is echoed verbatim from its SQE");
                continue;
            }
            if (!res_allowed(op, cq[i].res)) {
                note("opcode %u res %lld", op, (long long)cq[i].res);
                fail("every CQE res is in its opcode's allowed set");
            }
            atomic_fetch_add(&op_total[op & 15], 1);
            if (cq[i].res >= 0)
                atomic_fetch_add(&op_ok[op & 15], 1);
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

/* A buffer whose second page is unmapped, so a copy off the end faults. */
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

/* Faulting SQ/CQ buffers, malformed ioctls and rival SETUPs. */
static void *chaos(void *arg)
{
    uint64_t seed  = *(uint64_t *)arg;
    size_t usable  = 0;
    uint8_t *guard = guard_buf(&usable);
    struct koru_params p;
    struct koru_enter e;

    while (!atomic_load(&stop)) {
        if (guard) {
            size_t off = usable - rnd_below(&seed, 96);

            /* cq_space 0: a CQE this thread swallows is one the workers never
             * see, and a swallowed OPEN handle fills the table. */
            memset(&e, 0, sizeof(e));
            e.sq_addr   = (uint64_t)(uintptr_t)(guard + off);
            e.to_submit = 1 + rnd_below(&seed, 8);
            e.cq_space  = 0;
            memset(guard + off, 0, usable - off);
            account(ioctl(fd, KORU_IOC_ENTER, &e), &e);

            /* A CQ array that faults partway must leave the rest queued. */
            memset(&e, 0, sizeof(e));
            e.cq_addr  = (uint64_t)(uintptr_t)(guard + usable - 16);
            e.cq_space = 1 + rnd_below(&seed, 4);
            account(ioctl(fd, KORU_IOC_ENTER, &e), &e);
        }

        memset(&e, 0, sizeof(e));
        e.sq_addr   = 0x10;
        e.cq_addr   = 0x10;
        e.to_submit = 1;
        e.cq_space  = 1;
        account(ioctl(fd, KORU_IOC_ENTER, &e), &e);
        ioctl(fd, KORU_IOC_ENTER, (void *)0x10);
        ioctl(fd, KORU_IOC_SETUP, (void *)0x10);
        ioctl(fd, KORU_IOC_GET_PARAMS, (void *)0x10);

        {
            unsigned dir  = rnd_below(&seed, 4);
            unsigned nr   = rnd_below(&seed, 5);
            unsigned size = rnd_below(&seed, 2) ? rnd_below(&seed, 200) : 104;
            unsigned cmd = (dir << _IOC_DIRSHIFT) | ('k' << _IOC_TYPESHIFT) | (nr << _IOC_NRSHIFT) |
                           (size << _IOC_SIZESHIFT);

            memset(&p, 0, sizeof(p));
            ioctl(fd, cmd, &p);
            cmd = (dir << _IOC_DIRSHIFT) | ('z' << _IOC_TYPESHIFT) | (nr << _IOC_NRSHIFT) |
                  (size << _IOC_SIZESHIFT);
            if (ioctl(fd, cmd, &p) == 0 || errno != ENOTTY)
                fail("an unknown ioctl type byte is ENOTTY");
        }

        memset(&p, 0, sizeof(p));
        p.magic       = KORU_MAGIC;
        p.abi_version = KORU_ABI_VERSION;
        p.sq_entries  = F_SQ;
        p.cq_entries  = F_CQ;
        p.slot_size   = F_SLOT;
        p.slot_count  = F_SLOTS;
        if (ioctl(fd, KORU_IOC_SETUP, &p) == 0 || errno != EBUSY)
            fail("a second SETUP on a configured ring is EBUSY");

        if (mmap(NULL, F_ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) != MAP_FAILED)
            fail("a second mmap of the arena is refused");

        usleep(1000);
    }
    if (guard)
        munmap(guard, (size_t)sysconf(_SC_PAGESIZE) * 2);
    return NULL;
}

/* Its own ring, unmapped under in-flight work, then a clean exit or a SIGKILL.
 * This is what drives release() under load. */
static void child_body(uint64_t seed)
{
    struct koru_sqe sq[BATCH];
    struct koru_cqe cq[16];
    struct koru_enter e;
    struct koru_params p;
    uint8_t *a;
    unsigned i;
    int cfd = open(KORU_DEV, O_RDWR);

    if (cfd < 0)
        _exit(1);

    memset(&p, 0, sizeof(p));
    p.magic        = KORU_MAGIC;
    p.abi_version  = KORU_ABI_VERSION;
    p.sq_entries   = one_in(&seed, 8) ? rnd_below(&seed, 9000) : F_SQ;
    p.cq_entries   = F_CQ;
    p.slot_size    = one_in(&seed, 8) ? rnd_below(&seed, 9000) : F_SLOT;
    p.slot_count   = F_SLOTS;
    p.handle_count = one_in(&seed, 8) ? rnd_below(&seed, 9000) : F_HANDLES;
    if (ioctl(cfd, KORU_IOC_SETUP, &p) != 0)
        _exit(0); /* a rejected SETUP is a fine outcome */

    a = mmap(NULL, F_ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
    if (a == MAP_FAILED)
        _exit(0);
    put_path(a, F_SLOT, 0, PATFILE);

    for (i = 0; i < BATCH; i++)
        gen_valid(&seed, &sq[i], i, 0);
    sqe_delay(&sq[0], 0x100, (rnd_below(&seed, 500) + 100) * MS);
    sqe_checksum(&sq[1], 0, 0, F_SLOT, 0x101);

    memset(&e, 0, sizeof(e));
    e.sq_addr   = (uint64_t)(uintptr_t)sq;
    e.cq_addr   = (uint64_t)(uintptr_t)cq;
    e.to_submit = BATCH;
    e.cq_space  = 16;
    ioctl(cfd, KORU_IOC_ENTER, &e);

    if (one_in(&seed, 2))
        munmap(a, F_ARENA);
    if (one_in(&seed, 3))
        close(cfd);
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

/* No SA_RESTART, so a blocked ENTER returns EINTR. */
static void usr1(int sig)
{
    (void)sig;
}

static unsigned long long drain(void)
{
    struct koru_cqe cq[F_CQ];
    struct koru_enter e;
    unsigned long long got = 0;
    unsigned i;
    int idle = 0;

    while (idle < 20) {
        memset(&e, 0, sizeof(e));
        e.cq_addr  = (uint64_t)(uintptr_t)cq;
        e.cq_space = F_CQ;
        if (ioctl(fd, KORU_IOC_ENTER, &e) < 0)
            break;
        if (e.completed == 0) {
            idle++;
            usleep(20000);
            continue;
        }
        idle = 0;
        for (i = 0; i < e.completed; i++)
            check_cqe(&cq[i]);
        got += e.completed;
    }
    return got;
}

int fuzz_main(unsigned secs, uint64_t seed)
{
    static const char *names[11] = { "NOP",   "DELAY", "OPEN",  "READ",  "CLOSE", "CANCEL",
                                     "CKSUM", "WRITE", "ADOPT", "POLL",  "other" };
    struct koru_ring m;
    pthread_t th[NWORKERS + 2];
    uint64_t seeds[NWORKERS + 2];
    struct wctx wc[NWORKERS];
    struct koru_params p;
    struct sigaction sa;
    uint64_t deadline;
    int i, op, nth = 0, reached;

    note("%u seconds, seed %llu", secs, (unsigned long long)seed);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1;
    sigaction(SIGUSR1, &sa, NULL);

    if (make_pattern_file(FUZZWRFILE, F_SLOT) != 0) {
        fail("create the fuzzer's scratch file");
        return failures;
    }
    adoptable = open(PATFILE, O_RDONLY);
    if (adoptable < 0) {
        fail("open the fuzzer's adoptable fd");
        return failures;
    }

    if (ring_open(&m, F_SQ, F_CQ, F_SLOT, F_SLOTS, F_HANDLES) != 0 || ring_map(&m) != 0) {
        failures++;
        unlink(FUZZWRFILE);
        return failures;
    }
    fd    = m.fd;
    arena = m.arena;

    memset(&p, 0, sizeof(p));
    if (ioctl(fd, KORU_IOC_GET_PARAMS, &p) != 0 || p.max_delay_ns == 0) {
        fail("GET_PARAMS reports max_delay_ns");
        ring_close(&m);
        return failures;
    }
    max_delay_ns = p.max_delay_ns;

    for (i = 0; i < NWORKERS; i++) {
        wc[i].seed      = seed + (uint64_t)i * 0x9e3779b97f4a7c15ull;
        wc[i].path_slot = F_SLOTS - 1 - (uint32_t)i; /* one private slot each */
        pthread_create(&th[nth], NULL, worker, &wc[i]);
        nth++;
    }
    seeds[nth] = seed ^ 0xfeedfaceull;
    pthread_create(&th[nth], NULL, chaos, &seeds[nth]);
    nth++;
    seeds[nth] = seed ^ 0xdeadbeefull;
    pthread_create(&th[nth], NULL, spawner, &seeds[nth]);
    nth++;

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

        note("%llu ENTERs, %llu SQEs consumed, %llu CQEs reaped (%llu at drain)",
             (unsigned long long)atomic_load(&total_ops), sub, rea, tail);
        for (op = 0; op < 11; op++)
            note("%-6s %7llu completed, %7llu succeeded", names[op],
                 (unsigned long long)atomic_load(&op_total[op]),
                 (unsigned long long)atomic_load(&op_ok[op]));
        note("OPEN fails: EINVAL %llu EBUSY %llu EMFILE %llu other %llu",
             (unsigned long long)atomic_load(&open_einval),
             (unsigned long long)atomic_load(&open_ebusy),
             (unsigned long long)atomic_load(&open_emfile),
             (unsigned long long)atomic_load(&open_other));

        check(sub > SUB_FLOOR, "the fuzzer actually exercised the ring");
        /* Completion, not success: the tail sections carry that claim. */
        reached = 1;
        for (op = 0; op <= KORU_OP_POLL_ADD; op++)
            if (atomic_load(&op_total[op]) == 0) {
                note("opcode %d never completed once", op);
                reached = 0;
            }
        check(reached, "every opcode was reached at least once");
        check(sub == rea, "C1 holds: consumed SQEs == reaped CQEs");
        if (sub != rea)
            note("consumed %llu, reaped %llu", sub, rea);
    }

    ring_close(&m);
    close(adoptable);
    unlink(FUZZWRFILE);
    return failures;
}

void sec_fuzz(void)
{
    const char *env = getenv("KORU_SEED");
    uint64_t seed   = env ? strtoull(env, NULL, 0) : (uint64_t)wall_ms() * 2654435761u;
    pid_t pid;
    int st = 0;

    if (seed == 0)
        seed = 1;

    pid = fork();
    if (pid == 0) {
        int n = fuzz_main(SECONDS, seed);

        _exit(n > 100 ? 100 : n);
    }
    if (pid < 0 || waitpid(pid, &st, 0) != pid) {
        perror("fork");
        failures++;
        return;
    }
    if (!WIFEXITED(st)) {
        check(0, "the fuzzer exited cleanly");
        note("killed by signal %d", WTERMSIG(st));
        return;
    }
    if (WEXITSTATUS(st) != 0) {
        failures += WEXITSTATUS(st);
        note("fuzz seed %llu: replay with KORU_SEED=%llu", (unsigned long long)seed,
             (unsigned long long)seed);
    }
}
