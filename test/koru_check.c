// SPDX-License-Identifier: MIT
//
// koru_check: the integrated test for the kernel module. One process, one
// shared ring, run inside the virtme-ng guest by scripts/check.sh.
//
// Section ordering, budgets and what the reductions cost: doc/Notes.md.
//
// Usage:
//   koru_check                  everything, in order
//   koru_check <section>...     just those, for a development loop
//   koru_check --list           the section names
//   koru_check hold             hold a ring open and wait
//   koru_check pending          queue delays and exit at once
//
// KORU_SEED=<n> replays a fuzz failure. There is no duration knob.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "koru_check.h"

struct koru_ring R = { .fd = -1 };

/* ---------------------------------------------------------------------------
 * Sections.
 * ------------------------------------------------------------------------ */

struct sec {
    const char *name;
    void (*fn)(void);
    int heavy; /* allocates in bulk: must run early enough for kmemleak */
};

static const struct sec sections[] = {
    { "smoke", sec_smoke, 0 },

    /* Heavy: everything that allocates in bulk. */
    { "devchurn", sec_devchurn, 1 },
    { "ringchurn", sec_ringchurn, 1 },
    { "handles", sec_handles, 1 },
    { "fuzz", sec_fuzz, 1 },
    { "races", sec_races, 1 },
    { "write", sec_write, 1 },
    { "adopt", sec_adopt, 1 },
    { "path", sec_path, 1 },

    /* Tail: deterministic matrices and the timing checks. */
    { "setup", sec_setup, 0 },
    { "ioctl", sec_ioctl, 0 },
    { "mmap", sec_mmap, 0 },
    { "enter", sec_enter, 0 },
    { "slots", sec_slots, 0 },
    { "checksum", sec_checksum, 0 },
    { "open", sec_open, 0 },
    { "read", sec_read, 0 },
    { "nonblock", sec_nonblock, 0 },
    { "delay", sec_delay, 0 },
    { "cancel", sec_cancel, 0 },
    { "poll", sec_poll, 0 },
    { "stat", sec_stat, 0 },
    { "signals", sec_signals, 0 },
    { "creds", sec_creds, 0 },
};

#define NSECTIONS (sizeof(sections) / sizeof(sections[0]))

/* ------------------------------------------------------------------------- */

void sec_smoke(void)
{
    struct koru_sqe s;
    unsigned i;
    int zero = 1;

    sqe_nop(&s, 0x01);
    check_res(run_one(R.fd, &s), 0, "a NOP completes");

    /* Zeroed at SETUP, not at mmap. Nothing has written here yet. */
    for (i = 0; i < R.slot_size; i++)
        if (R.arena[R.slot_size + i] != 0)
            zero = 0;
    check(zero, "the arena comes back zeroed from SETUP");

    memset(R.arena, 0xa5, 4096);
    sqe_checksum(&s, 0, 0, 4096, 0x02);
    check_res(run_one(R.fd, &s), fnv1a(R.arena, 4096), "a CHECKSUM reads the arena");
}

/* ---------------------------------------------------------------------------
 * SETUP and GET_PARAMS.
 * ------------------------------------------------------------------------ */

static void good_request(struct koru_params *p)
{
    memset(p, 0, sizeof(*p));
    p->magic       = KORU_MAGIC;
    p->abi_version = KORU_ABI_VERSION;
    p->sq_entries  = 64;
    p->cq_entries  = 128;
    p->slot_size   = 4096;
    p->slot_count  = 32;
}

/* Every rejection runs on one fd; the SETUP at the end proves none consumed
 * the one-shot. */
static int fd_reject;

static void reject(void (*mutate)(struct koru_params *), int want, const char *what)
{
    struct koru_params p;

    good_request(&p);
    mutate(&p);
    check_errno(ioctl(fd_reject, KORU_IOC_SETUP, &p), want, what);
}

static struct koru_params caps;

static void bad_magic(struct koru_params *p) { p->magic = 0xdeadbeef; }
static void bad_version(struct koru_params *p) { p->abi_version = 999; }
static void bad_flags(struct koru_params *p) { p->flags = 1; }
static void bad_reserved(struct koru_params *p) { p->reserved[1] = 1; }
static void zero_sq(struct koru_params *p) { p->sq_entries = 0; }
static void big_sq(struct koru_params *p) { p->sq_entries = caps.max_sq_entries + 1; }
static void big_cq(struct koru_params *p) { p->cq_entries = caps.max_cq_entries + 1; }
static void shallow_cq(struct koru_params *p) { p->cq_entries = 32; }
static void zero_slot_size(struct koru_params *p) { p->slot_size = 0; }
static void odd_slot_size(struct koru_params *p) { p->slot_size = 100; }
static void unaligned_slot_size(struct koru_params *p) { p->slot_size = 4097; }
static void big_slot_size(struct koru_params *p) { p->slot_size = caps.max_slot_size + 1; }
static void zero_slot_count(struct koru_params *p) { p->slot_count = 0; }
static void big_slot_count(struct koru_params *p) { p->slot_count = caps.max_slot_count + 1; }
static void big_handles(struct koru_params *p) { p->handle_count = caps.max_handles + 1; }

static void big_arena(struct koru_params *p)
{
    p->slot_size  = caps.max_slot_size;
    p->slot_count = caps.max_slot_count;
}

void sec_setup(void)
{
    struct koru_params p, q;
    int fd;

    fd = open_dev();
    if (fd < 0)
        return;

    /* 1. GET_PARAMS is legal before SETUP and reports the caps. */
    memset(&caps, 0, sizeof(caps));
    check(ioctl(fd, KORU_IOC_GET_PARAMS, &caps) == 0, "GET_PARAMS before SETUP succeeds");
    check(caps.magic == KORU_MAGIC && caps.abi_version == KORU_ABI_VERSION,
          "  it reports magic and abi_version");
    check(caps.configured == 0 && caps.handle_count == 0, "  configured == 0 before SETUP");
    check(caps.max_sq_entries > 0 && caps.max_cq_entries > 0 && caps.max_slot_size > 0 &&
              caps.max_slot_count > 0 && caps.max_arena_bytes > 0 && caps.max_handles > 0 &&
              caps.max_delay_ns > 0,
          "  every cap is non-zero");

    /* 2. Every rejection, on one fd. */
    fd_reject = fd;
    reject(bad_magic, EPROTO, "bad magic is EPROTO, not EINVAL");
    reject(bad_version, EPROTO, "bad abi_version is EPROTO");
    reject(bad_flags, EINVAL, "an unknown SETUP flag bit is EINVAL");
    reject(bad_reserved, EINVAL, "a non-zero reserved word is EINVAL");
    reject(zero_sq, EINVAL, "sq_entries 0 is EINVAL");
    reject(big_sq, EINVAL, "sq_entries past the cap is EINVAL");
    reject(big_cq, EINVAL, "cq_entries past the cap is EINVAL");
    reject(shallow_cq, EINVAL, "cq_entries below sq_entries is EINVAL");
    reject(zero_slot_size, EINVAL, "slot_size 0 is EINVAL");
    reject(odd_slot_size, EINVAL, "slot_size 100 is EINVAL");
    reject(unaligned_slot_size, EINVAL, "slot_size 4097 is EINVAL");
    reject(big_slot_size, EINVAL, "slot_size past the cap is EINVAL");
    reject(zero_slot_count, EINVAL, "slot_count 0 is EINVAL");
    reject(big_slot_count, EINVAL, "slot_count past the cap is EINVAL");
    reject(big_arena, EINVAL, "an arena past the cap is rejected, not clamped");
    reject(big_handles, EINVAL, "handle_count past the cap is EINVAL");

    /* 3. Which leaves the fd still virgin. */
    good_request(&p);
    check(ioctl(fd, KORU_IOC_SETUP, &p) == 0, "SETUP still works after every rejection");
    check(p.sq_entries == 64 && p.cq_entries == 128, "  sq and cq entries round-trip");
    check(p.slot_size == 4096 && p.slot_count == 32, "  slot size and count round-trip");
    check(p.arena_size == (uint64_t)4096 * 32, "  arena_size is slot_size * slot_count");
    check(p.configured == 1, "  configured == 1 after SETUP");
    check(p.handle_count > 0 && p.handle_count <= caps.max_handles,
          "  handle_count 0 became the kernel default");

    /* 4. GET_PARAMS afterwards agrees, field for field. */
    memset(&q, 0, sizeof(q));
    check(ioctl(fd, KORU_IOC_GET_PARAMS, &q) == 0, "GET_PARAMS after SETUP succeeds");
    check(memcmp(&p, &q, sizeof(p)) == 0, "  and returns what SETUP returned");

    /* 5. The one-shot is now spent. */
    good_request(&p);
    check_errno(ioctl(fd, KORU_IOC_SETUP, &p), EBUSY, "a second SETUP is EBUSY");
    close(fd);

    /* 6. Defaults and echoes that need their own fds. */
    fd = open_dev();
    if (fd >= 0) {
        good_request(&p);
        p.cq_entries = 0;
        check(ioctl(fd, KORU_IOC_SETUP, &p) == 0 && p.cq_entries == p.sq_entries,
              "cq_entries 0 becomes sq_entries");
        close(fd);
    }
    fd = open_dev();
    if (fd >= 0) {
        good_request(&p);
        p.handle_count = 7;
        check(ioctl(fd, KORU_IOC_SETUP, &p) == 0 && p.handle_count == 7,
              "an explicit handle_count is echoed back");
        close(fd);
    }
}

/* ---------------------------------------------------------------------------
 * ioctl dispatch: ENOTTY for "not ours", EPROTO for "ours, malformed".
 * ------------------------------------------------------------------------ */

#define IOC(dir, type, nr, size)                                                                   \
    (((unsigned)(dir) << _IOC_DIRSHIFT) | ((unsigned)(type) << _IOC_TYPESHIFT) |                   \
     ((unsigned)(nr) << _IOC_NRSHIFT) | ((unsigned)(size) << _IOC_SIZESHIFT))

void sec_ioctl(void)
{
    struct koru_params p;
    unsigned dir, nr;
    int bad_type = 0, bad_nr = 0;

    memset(&p, 0, sizeof(p));

    /* Dispatch is on the type byte first, then the number. */
    check_errno(ioctl(R.fd, _IO('x', 0x7f)), ENOTTY, "an unknown ioctl is ENOTTY");
    for (dir = 0; dir < 4; dir++)
        for (nr = 0; nr < 5; nr++)
            if (ioctl(R.fd, IOC(dir, 'z', nr, sizeof(p)), &p) == 0 || errno != ENOTTY)
                bad_type = 1;
    check(!bad_type, "a foreign type byte is always ENOTTY");

    for (nr = 3; nr < 8; nr++)
        if (ioctl(R.fd, IOC(_IOC_READ | _IOC_WRITE, 'k', nr, sizeof(p)), &p) == 0 ||
            errno != ENOTTY)
            bad_nr = 1;
    check(!bad_nr, "our type byte with an unknown number is ENOTTY");

    /* Right number, wrong size or direction: an ABI mismatch, so EPROTO. */
    check_errno(ioctl(R.fd, IOC(_IOC_READ | _IOC_WRITE, 'k', 0x00, sizeof(p) + 8), &p), EPROTO,
                "SETUP with the wrong size is EPROTO");
    check_errno(ioctl(R.fd, IOC(_IOC_READ, 'k', 0x00, sizeof(p)), &p), EPROTO,
                "SETUP encoded read-only is EPROTO");
    check_errno(ioctl(R.fd, IOC(_IOC_WRITE, 'k', 0x01, sizeof(p)), &p), EPROTO,
                "GET_PARAMS encoded write-only is EPROTO");
    check_errno(ioctl(R.fd, IOC(_IOC_NONE, 'k', 0x01, sizeof(p)), &p), EPROTO,
                "  and with no direction at all");
    check_errno(ioctl(R.fd, IOC(_IOC_WRITE, 'k', 0x02, sizeof(struct koru_enter)), &p), EPROTO,
                "ENTER encoded write-only is EPROTO");

    check_errno(ioctl(R.fd, KORU_IOC_GET_PARAMS, (void *)0x10), EFAULT,
                "GET_PARAMS with a bad pointer is EFAULT");
    check_errno(ioctl(R.fd, KORU_IOC_ENTER, (void *)0x10), EFAULT,
                "ENTER with a bad pointer is EFAULT");
    check_errno(ioctl(R.fd, KORU_IOC_SETUP, (void *)0x10), EFAULT,
                "SETUP with a bad pointer is EFAULT");
}

/* ---------------------------------------------------------------------------
 * The arena mapping.
 * ------------------------------------------------------------------------ */

#define M_SLOT  4096u
#define M_COUNT 8u
#define M_ARENA ((size_t)M_SLOT * M_COUNT)

static int region_in_maps(void)
{
    char line[512];
    FILE *f   = fopen("/proc/self/maps", "r");
    int found = 0;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "/dev/koru"))
            found = 1;
    fclose(f);
    return found;
}

void sec_mmap(void)
{
    struct koru_ring m;
    struct koru_sqe s;
    struct koru_enter e;
    uint8_t *a, *second;
    uint8_t pattern[M_SLOT];
    unsigned i;
    int fd, status = 0;
    pid_t pid;

    for (i = 0; i < M_SLOT; i++)
        pattern[i] = (uint8_t)(i * 31 + 7);

    /* 1. Before SETUP there is no arena to map. */
    fd = open_dev();
    if (fd < 0)
        return;
    a = mmap(NULL, M_ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(a == MAP_FAILED && errno == EINVAL, "mmap before SETUP is EINVAL");
    close(fd);

    if (ring_open(&m, 32, 64, M_SLOT, M_COUNT, 8) != 0) {
        failures++;
        return;
    }

    /* 2. Length is exact, not a minimum. */
    a = mmap(NULL, M_ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE, m.fd, 0);
    check(a == MAP_FAILED && errno == EINVAL, "MAP_PRIVATE is EINVAL, never a silent COW");
    a = mmap(NULL, M_ARENA - 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m.fd, 0);
    check(a == MAP_FAILED && errno == EINVAL, "a mapping one page short is EINVAL");
    a = mmap(NULL, M_ARENA + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m.fd, 0);
    check(a == MAP_FAILED && errno == EINVAL, "a mapping one page long is EINVAL");
    a = mmap(NULL, M_ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, m.fd, 4096);
    check(a == MAP_FAILED && errno == EINVAL, "a non-zero file offset is EINVAL");

    if (ring_map(&m) != 0) {
        failures++;
        ring_close(&m);
        return;
    }
    check(1, "an exact MAP_SHARED mapping succeeds");
    second = mmap(NULL, M_ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, m.fd, 0);
    check(second == MAP_FAILED && errno == EBUSY, "a second mmap is EBUSY");

    /* 3. VM_DONTCOPY, and userspace cannot undo it. */
    check(region_in_maps() == 1, "the region shows in /proc/self/maps");
    check(madvise(m.arena, M_ARENA, MADV_DOFORK) != 0, "madvise(MADV_DOFORK) is refused");
    pid = fork();
    if (pid == 0)
        _exit(region_in_maps() == 0 ? 0 : 1);
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a forked child does not inherit it");

    /* 4. The kernel owns the pages. */
    sqe_delay(&s, 0xd1, 100 * MS);
    enter_init(&e, &s, 1, NULL, 0);
    check(ioctl(m.fd, KORU_IOC_ENTER, &e) == 1, "a delay is in flight");
    check(munmap(m.arena, M_ARENA) == 0, "munmap with an op in flight succeeds");
    m.arena = NULL;
    check(ring_quiesce(&m) == 1, "  and the op still lands afterwards");
    ring_close(&m);

    /* 5. The mapping holds its own reference. */
    if (ring_open(&m, 32, 64, M_SLOT, M_COUNT, 8) != 0) {
        failures++;
        return;
    }
    if (ring_map(&m) != 0) {
        failures++;
        ring_close(&m);
        return;
    }
    memcpy(m.arena + 2 * M_SLOT, pattern, M_SLOT);
    a = m.arena;
    close(m.fd);
    m.fd = -1;
    check(memcmp(a + 2 * M_SLOT, pattern, M_SLOT) == 0, "the mapping still reads after close(fd)");
    check(munmap(a, M_ARENA) == 0, "munmap after close(fd) succeeds");
    m.arena = NULL;
    ring_close(&m);
}

/* ---------------------------------------------------------------------------
 * The ENTER protocol: C1, E1, admission control.
 * ------------------------------------------------------------------------ */

/* Every opcode, plus two that do not exist. */
static const uint8_t all_opcodes[] = { KORU_OP_NOP,      KORU_OP_DELAY_NS, KORU_OP_OPEN,
                                       KORU_OP_READ,     KORU_OP_CLOSE,    KORU_OP_CANCEL,
                                       KORU_OP_CHECKSUM, KORU_OP_WRITE,    KORU_OP_ADOPT_FD,
                                       KORU_OP_POLL_ADD, KORU_OP_STAT,     11,
                                       200 };

/* Submit and never reap. Admission control must stop this at cq_entries. */
static unsigned fill_cq(struct koru_ring *r, uint64_t delay_ns)
{
    struct koru_sqe sq[8];
    struct koru_enter e;
    unsigned total = 0, i, j;
    int ret;

    for (i = 0; i < r->cq_entries / 8 + 4; i++) {
        for (j = 0; j < 8; j++) {
            if (delay_ns)
                sqe_delay(&sq[j], 0x5000 + total + j, delay_ns);
            else
                sqe_nop(&sq[j], 0x5000 + total + j);
        }
        enter_init(&e, sq, 8, NULL, 0);
        ret = ioctl(r->fd, KORU_IOC_ENTER, &e);
        if (ret < 0) {
            perror("ENTER");
            failures++;
            break;
        }
        total += (unsigned)ret;
        if (ret < 8)
            break; /* short submit: the queue is full */
    }
    return total;
}

void sec_enter(void)
{
    struct koru_sqe sq[16];
    struct koru_cqe cq[16];
    struct koru_enter e;
    struct koru_ring a;
    unsigned i;
    int fd, ret, ok, res_ok;

    /* 1. Eight NOPs in, eight CQEs out, in order. */
    for (i = 0; i < 8; i++)
        sqe_nop(&sq[i], 0x1000 + i);
    enter_init(&e, sq, 8, cq, 16);
    ret = ioctl(R.fd, KORU_IOC_ENTER, &e);
    check(ret == 8, "ENTER returns the count of SQEs consumed");
    check(e.completed == 8 && e.submitted == 8, "  completed and submitted both 8");
    ok = res_ok = 1;
    for (i = 0; i < 8; i++) {
        if (cq[i].user_data != 0x1000 + i)
            ok = 0;
        if (cq[i].res != 0 || cq[i].flags != 0 || cq[i].rsvd0 != 0 || cq[i].extra != 0)
            res_ok = 0;
    }
    check(ok, "  user_data matches one for one, in order");
    check(res_ok, "  res 0, and flags, rsvd0 and extra all zero");

    /* 2. E1: a bad SQE is a completion, never an ioctl failure. */
    sqe_nop(&sq[0], 0x2000);
    sq[0].opcode = 200;
    enter_init(&e, sq, 1, cq, 16);
    ret = ioctl(R.fd, KORU_IOC_ENTER, &e);
    check(ret == 1 && e.completed == 1 && cq[0].user_data == 0x2000 && cq[0].res == -EINVAL,
          "an unknown opcode is a CQE with res -EINVAL, not an error");

    sqe_nop(&sq[0], 0x2003);
    sq[0].len = 1;
    enter_init(&e, sq, 1, cq, 16);
    ret = ioctl(R.fd, KORU_IOC_ENTER, &e);
    check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
          "a field the opcode does not read must be zero");

    /* 3. The two rules that let the ABI grow, for every opcode. */
    {
        int bad_rsvd = 0, bad_flags = 0;

        for (i = 0; i < sizeof(all_opcodes); i++) {
            sqe_nop(&sq[0], 0x2100 + i);
            sq[0].opcode = all_opcodes[i];
            sq[0].rsvd0  = 1;
            if (run_one(R.fd, &sq[0]) != -EINVAL)
                bad_rsvd = 1;

            sqe_nop(&sq[0], 0x2200 + i);
            sq[0].opcode = all_opcodes[i];
            sq[0].flags  = 1;
            if (run_one(R.fd, &sq[0]) != -EINVAL)
                bad_flags = 1;
        }
        check(!bad_rsvd, "a non-zero SQE rsvd0 is EINVAL for every opcode");
        check(!bad_flags, "an unknown SQE flag bit is EINVAL for every opcode");
    }

    /* 4. C1: a mixed batch completes every entry it consumed. */
    for (i = 0; i < 8; i++) {
        sqe_nop(&sq[i], 0x3000 + i);
        if (i % 2)
            sq[i].opcode = 250;
    }
    enter_init(&e, sq, 8, cq, 16);
    ret = ioctl(R.fd, KORU_IOC_ENTER, &e);
    check(ret == 8 && e.completed == 8, "a mixed valid/invalid batch completes all 8 (C1)");
    ok = 1;
    for (i = 0; i < 8; i++) {
        int64_t want = (i % 2) ? -EINVAL : 0;

        if (cq[i].user_data != 0x3000 + i || cq[i].res != want)
            ok = 0;
    }
    check(ok, "  each completion matches its own SQE");

    /* 5. A short cq_space queues the rest rather than dropping it. */
    for (i = 0; i < 8; i++)
        sqe_nop(&sq[i], 0x4000 + i);
    enter_init(&e, sq, 8, cq, 3);
    ret = ioctl(R.fd, KORU_IOC_ENTER, &e);
    check(ret == 8 && e.completed == 3, "cq_space 3 of 8 consumes 8 and completes 3");
    memset(cq, 0, sizeof(cq));
    enter_init(&e, NULL, 0, cq, 16);
    ret = ioctl(R.fd, KORU_IOC_ENTER, &e);
    check(ret == 0 && e.completed == 5, "  a later ENTER returns the other 5");
    ok = 1;
    for (i = 0; i < 5; i++)
        if (cq[i].user_data != 0x4003 + i)
            ok = 0;
    check(ok, "  in order, and they are the ones left behind");

    /* 6. Protocol failures that do fail the ioctl. */
    enter_init(&e, sq, 1, cq, 16);
    e.flags = 1;
    check_errno(ioctl(R.fd, KORU_IOC_ENTER, &e), EINVAL, "an unknown ENTER flag is EINVAL");
    enter_init(&e, sq, 1, cq, 16);
    e.reserved[1] = 1;
    check_errno(ioctl(R.fd, KORU_IOC_ENTER, &e), EINVAL, "a non-zero ENTER reserved word is EINVAL");
    enter_init(&e, sq, R.sq_entries + 1, cq, 16);
    check_errno(ioctl(R.fd, KORU_IOC_ENTER, &e), EINVAL, "to_submit past sq_entries is EINVAL");
    enter_init(&e, sq, 1, cq, R.cq_entries + 1);
    check_errno(ioctl(R.fd, KORU_IOC_ENTER, &e), EINVAL, "cq_space past cq_entries is EINVAL");
    enter_init(&e, sq, 1, cq, 2);
    e.min_complete = 3;
    check_errno(ioctl(R.fd, KORU_IOC_ENTER, &e), EINVAL, "min_complete past cq_space is EINVAL");
    enter_init(&e, sq, 1, NULL, 0);
    e.sq_addr = 0x10;
    check_errno(ioctl(R.fd, KORU_IOC_ENTER, &e), EFAULT, "an unmapped sq_addr is EFAULT");

    fd = open_dev();
    if (fd >= 0) {
        enter_init(&e, sq, 0, cq, 16);
        check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EINVAL, "ENTER before SETUP is EINVAL");
        close(fd);
    }

    /* 7. Admission control: queued completions, then reservations. */
    if (ring_open(&a, 64, 128, 4096, 8, 8) == 0) {
        check(fill_cq(&a, 0) == a.cq_entries, "submitting past cq_entries stops at a short count");
        ring_close(&a);
    }
    if (ring_open(&a, 64, 128, 4096, 8, 8) == 0) {
        check(fill_cq(&a, 2 * MS) == a.cq_entries, "in-flight work counts against cq_entries too");
        ring_close(&a);
    }
}

/* Its own ring: 80 slots, so the busy bitmap spans two 64-bit words. */

#define S_SLOT  4096u
#define S_COUNT 80u

/* Two deferred CHECKSUMs on one slot, retried until they actually collide.
 *
 * The collision is racy: the winner's kworker can finish before the submit loop
 * dispatches the loser, and then both succeed. Exclusivity is still what is
 * being tested — if it were broken, no attempt would ever collide and this
 * returns 0. `*winner` is the successful checksum from the colliding round.
 */
static int collide(struct koru_ring *m, uint32_t slot, unsigned n, int64_t *winner)
{
    struct koru_sqe sq[3];
    struct koru_cqe cq[3];
    unsigned completed = 0, i;
    int attempt, ok, busy;

    for (attempt = 1; attempt <= 64; attempt++) {
        for (i = 0; i < n; i++)
            sqe_checksum(&sq[i], slot, 0, S_SLOT, 0x20 + i);
        if (submit(m->fd, sq, n, cq, n, n, &completed) != (int)n || completed != n)
            return 0;
        ok = busy = 0;
        for (i = 0; i < completed; i++) {
            if (cq[i].res >= 0) {
                ok++;
                if (winner)
                    *winner = cq[i].res;
            } else if (cq[i].res == -EBUSY) {
                busy++;
            }
        }
        if (ok == 1 && busy == (int)n - 1)
            return attempt;
    }
    return 0;
}

void sec_slots(void)
{
    struct koru_ring m;
    struct koru_sqe sq[8];
    struct koru_cqe cq[8];
    const struct koru_cqe *a, *b;
    uint8_t pattern[S_SLOT];
    unsigned completed = 0, i;
    int ret, ok, n;
    int64_t want, got;

    for (i = 0; i < S_SLOT; i++)
        pattern[i] = (uint8_t)(i * 17 + 3);
    want = fnv1a(pattern, S_SLOT);

    if (ring_open(&m, 32, 64, S_SLOT, S_COUNT, 8) != 0 || ring_map(&m) != 0) {
        failures++;
        ring_close(&m);
        return;
    }
    memcpy(m.arena + 3 * S_SLOT, pattern, S_SLOT);

    /* 1. Two ops naming one slot. CHECKSUM is deferred so this is reachable. */
    sqe_checksum(&sq[0], 3, 0, S_SLOT, 0xaa);
    sqe_checksum(&sq[1], 3, 0, S_SLOT, 0xbb);
    ret = submit(m.fd, sq, 2, cq, 2, 2, &completed);
    check(ret == 2 && completed == 2, "two ops on one slot: both consumed, both complete");
    a = find_cqe(cq, completed, 0xaa);
    b = find_cqe(cq, completed, 0xbb);
    check(a && b, "  each completion carries its own user_data");

    got = -1;
    n   = collide(&m, 3, 2, &got);
    check(n > 0, "  exactly one succeeds, the other gets -EBUSY");
    check(got == want, "  and the winner's checksum is right");
    if (n > 1)
        note("the collision took %d attempts", n);

    /* 2. Released in the same critical section that posts the CQE. */
    sqe_checksum(&sq[0], 3, 0, S_SLOT, 0xcc);
    check_res(run_one(m.fd, &sq[0]), want, "the slot is free again once its CQE posted");

    /* 3. Distinct slots do not collide. */
    memcpy(m.arena + 4 * S_SLOT, pattern, S_SLOT);
    sqe_checksum(&sq[0], 3, 0, S_SLOT, 0x10);
    sqe_checksum(&sq[1], 4, 0, S_SLOT, 0x11);
    ret = submit(m.fd, sq, 2, cq, 2, 2, &completed);
    a   = find_cqe(cq, completed, 0x10);
    b   = find_cqe(cq, completed, 0x11);
    check(ret == 2 && completed == 2 && a && b && a->res >= 0 && b->res >= 0,
          "two ops on different slots both succeed");

    /* 4. A refused op must not release the slot the winner holds. */
    check(collide(&m, 3, 3, NULL) > 0, "three on one slot: one succeeds, two get -EBUSY");

    /* 5. Past the first bitmap word, and no aliasing with the word below. */
    memcpy(m.arena + 70 * S_SLOT, pattern, S_SLOT);
    check(collide(&m, 70, 2, NULL) > 0,
          "slot 70, in the second bitmap word, is tracked separately");
    sqe_checksum(&sq[0], 6, 0, S_SLOT, 0x32);
    sqe_checksum(&sq[1], 70, 0, S_SLOT, 0x33);
    ret = submit(m.fd, sq, 2, cq, 2, 2, &completed);
    a   = find_cqe(cq, completed, 0x32);
    b   = find_cqe(cq, completed, 0x33);
    check(ret == 2 && completed == 2 && a && b && a->res >= 0 && b->res >= 0,
          "  and slots 6 and 70 do not alias each other");

    /* 6. slot_try_acquire does not bounds-check itself; every caller must.
     * The bitmap is whole words, so probe past the count and past word 0. */
    sqe_checksum(&sq[0], S_COUNT, 0, 16, 0x40);
    check_res(run_one(m.fd, &sq[0]), -EINVAL, "slot == slot_count is EINVAL");
    sqe_checksum(&sq[0], S_COUNT + 1, 0, 16, 0x41);
    check_res(run_one(m.fd, &sq[0]), -EINVAL, "  and one past that, still inside the bitmap");
    sqe_checksum(&sq[0], UINT32_MAX, 0, 16, 0x42);
    check_res(run_one(m.fd, &sq[0]), -EINVAL, "  and an absurd slot index");

    /* 7. A rejected op leaves no slot stuck busy. */
    sqe_checksum(&sq[0], 5, 0, S_SLOT, 0x50);
    sq[0].off = S_SLOT;
    check_res(run_one(m.fd, &sq[0]), -EINVAL, "an out-of-range op is rejected before it claims");
    sqe_checksum(&sq[0], 5, 0, S_SLOT, 0x51);
    check(run_one(m.fd, &sq[0]) >= 0, "  and slot 5 is still usable");

    /* 8. Every slot at once. */
    for (i = 0; i < 8; i++)
        sqe_checksum(&sq[i], i, 0, S_SLOT, 0x60 + i);
    ret = submit(m.fd, sq, 8, cq, 8, 8, &completed);
    ok  = 0;
    for (i = 0; i < completed; i++)
        if (cq[i].res >= 0)
            ok++;
    check(ret == 8 && completed == 8 && ok == 8, "eight distinct slots all succeed together");

    ring_close(&m);
}

/* ------------------------------------------------------------------------- */

/* rmmod must be refused while this runs. */
static int mode_hold(void)
{
    struct koru_ring m;

    if (ring_open(&m, 32, 64, 4096, 8, 8) != 0)
        return 1;
    printf("READY\n");
    fflush(stdout);
    for (;;)
        pause();
}

/* release() must cancel these, so the script's rmmod beats the delay. It must
 * also disarm the poll: the child pokes the pipe once the ring is gone, and
 * an entry left on that waitqueue is a use-after-free from the wake. */
static int mode_pending(void)
{
    struct koru_ring m;
    struct koru_sqe sq[4];
    struct koru_enter e;
    int pipefd[2];
    int64_t h;
    unsigned i;

    if (ring_open(&m, 32, 64, 4096, 8, 8) != 0)
        return 1;
    if (pipe2(pipefd, O_NONBLOCK) == 0) {
        h = r_adopt(&m, pipefd[0]);
        if (h > 0) {
            sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_IN, 0x7100);
            enter_init(&e, sq, 1, NULL, 0);
            if (ioctl(m.fd, KORU_IOC_ENTER, &e) != 1)
                return 1;
            if (fork() == 0) {
                close(m.fd); /* the child must not hold the ring open */
                close(pipefd[0]);
                usleep(200000);
                if (write(pipefd[1], "x", 1) != 1)
                    _exit(1);
                _exit(0);
            }
        }
    }
    for (i = 0; i < 4; i++)
        sqe_delay(&sq[i], 0x7000 + i, 5000 * MS);
    enter_init(&e, sq, 4, NULL, 0);
    if (ioctl(m.fd, KORU_IOC_ENTER, &e) != 4)
        return 1;
    printf("READY\n");
    fflush(stdout);
    _exit(0); /* the fd closes with the process, the delays stay queued */
}

/* ---------------------------------------------------------------------------
 * Driver.
 * ------------------------------------------------------------------------ */

static int selected(const char *name, int argc, char **argv)
{
    int i;

    if (argc < 2)
        return 1;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t heavy_end = 0;
    unsigned i;

    test_begin(120);

    if (argc > 1 && strcmp(argv[1], "hold") == 0)
        return mode_hold();
    if (argc > 1 && strcmp(argv[1], "pending") == 0)
        return mode_pending();
    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        for (i = 0; i < NSECTIONS; i++)
            printf("%s%s\n", sections[i].name, sections[i].heavy ? "  (heavy)" : "");
        return 0;
    }

    if (make_pattern_file(PATFILE, PATSIZE) != 0)
        return 1;

    if (ring_open(&R, SHARED_SQ, SHARED_CQ, SHARED_SLOT, SHARED_SLOTS, SHARED_HANDLES) != 0 ||
        ring_map(&R) != 0) {
        printf("cannot set up the shared ring: the module is not usable\n");
        return 1;
    }

    for (i = 0; i < NSECTIONS; i++) {
        char what[80];
        int left;

        if (!selected(sections[i].name, argc, argv))
            continue;
        printf("\n== SECTION %s ==\n", sections[i].name);
        sections[i].fn();

        /* A section that walked away from queued work fails here, not later. */
        left = ring_quiesce(&R);
        if (left != 0) {
            snprintf(what, sizeof(what), "  %s left nothing in flight", sections[i].name);
            check(0, what);
            note("%d stray completion(s)", left);
        }
        if (sections[i].heavy)
            heavy_end = wall_ms();

        if (failures && i == 0) {
            printf("\nsmoke failed: stopping rather than reporting on a dud module\n");
            break;
        }
    }

    ring_close(&R);
    unlink(PATFILE);

    /* The script tops the kmemleak window up from here. */
    printf("\nKORU-HEAVY-END-MS %llu\n", (unsigned long long)(heavy_end ? heavy_end : wall_ms()));
    return test_end();
}
