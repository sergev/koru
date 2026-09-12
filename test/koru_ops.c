// SPDX-License-Identifier: MIT
//
// Per-opcode matrices, plus signals and credentials, which need a fork.
// Tail sections: they allocate almost nothing. See doc/Notes.md.

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "koru_check.h"

#define PATH_SLOT 0u /* every OPEN below reads its path from here */
#define PATH_MAX_ 4096u

/* ------------------------------------------------------------------------- */

void sec_checksum(void)
{
    struct koru_sqe s;
    uint8_t *pattern, *rot;
    uint32_t n = R.slot_size;
    unsigned i;
    int64_t want;

    pattern = malloc(n);
    rot     = malloc(n);
    if (!pattern || !rot) {
        failures++;
        free(pattern);
        free(rot);
        return;
    }
    for (i = 0; i < n; i++)
        pattern[i] = (uint8_t)(i * 31 + 7);
    want = fnv1a(pattern, n);

    memcpy(R.arena + 3 * n, pattern, n);
    sqe_checksum(&s, 3, 0, n, 0xc0de);
    check_res(run_one(R.fd, &s), want, "a whole-slot CHECKSUM matches FNV-1a on the host");

    sqe_checksum(&s, 2, 0, n, 0xc0de);
    check(run_one(R.fd, &s) != want, "  an untouched slot checksums differently");

    memcpy(R.arena, pattern, n);
    sqe_checksum(&s, 0, 0, n, 0xc0de);
    check_res(run_one(R.fd, &s), want, "  slot 0 too");

    memcpy(R.arena + (size_t)(R.slot_count - 1) * n, pattern, n);
    sqe_checksum(&s, R.slot_count - 1, 0, n, 0xc0de);
    check_res(run_one(R.fd, &s), want, "  and the last slot");

    /* Position-sensitive: the same bytes reordered must differ. */
    memcpy(rot, pattern + 1, n - 1);
    rot[n - 1] = pattern[0];
    memcpy(R.arena + 1 * n, rot, n);
    sqe_checksum(&s, 1, 0, n, 0xc0de);
    check_res(run_one(R.fd, &s), fnv1a(rot, n), "a rotated pattern gives a different checksum");

    sqe_checksum(&s, 3, 100, 1000, 0xc0de);
    check_res(run_one(R.fd, &s), fnv1a(pattern + 100, 1000), "a sub-range at off 100 len 1000");

    /* An empty range is legal. */
    sqe_checksum(&s, 3, 0, 0, 0xc0de);
    check_res(run_one(R.fd, &s), fnv1a(pattern, 0), "a zero-length CHECKSUM is the offset basis");

    sqe_checksum(&s, 0, n - 8, 16, 0xc0de);
    check_res(run_one(R.fd, &s), -EINVAL, "off + len past the slot end is EINVAL");
    sqe_checksum(&s, 0, UINT64_MAX, 16, 0xc0de);
    check_res(run_one(R.fd, &s), -EINVAL, "an off + len that overflows is EINVAL");
    sqe_checksum(&s, 0, 0, 16, 0xc0de);
    s.handle = 1;
    check_res(run_one(R.fd, &s), -EINVAL, "CHECKSUM with a non-zero handle is EINVAL");

    free(pattern);
    free(rot);
}

/* ------------------------------------------------------------------------- */

#define LINKPATH "/tmp/koru-check-symlink"
#define SHADOW   "/etc/shadow"

static void handle_matrix(void)
{
    struct koru_sqe s;
    int64_t h, h2;
    uint32_t idx, gen;

    h = r_open(&R, PATH_SLOT, HOSTNAME, KORU_O_RDONLY);
    check(h > 0, "OPEN " HOSTNAME " returns a handle");
    if (h <= 0)
        return;
    idx = KORU_HANDLE_INDEX(h);
    gen = KORU_HANDLE_GEN(h);
    /* Generations start at 1 and skip 0 on wrap, so a handle is never 0. */
    check(gen != 0 && idx < R.handle_count, "  index inside the table, generation non-zero");
    check_res(r_close(&R, (uint32_t)h), 0, "  CLOSE retires it");

    check_res(r_close(&R, (uint32_t)h), -EBADF, "a double CLOSE is EBADF");
    check_res(r_close(&R, 0), -EBADF, "CLOSE of handle 0 is EBADF");
    check_res(r_close(&R, idx | ((gen + 7) << 16)), -EBADF, "a stale generation is EBADF");
    check_res(r_close(&R, R.handle_count | (1u << 16)), -EBADF, "an index past the table is EBADF");

    h2 = r_open(&R, PATH_SLOT, HOSTNAME, KORU_O_RDONLY);
    check(h2 > 0 && KORU_HANDLE_INDEX(h2) == idx && KORU_HANDLE_GEN(h2) != gen,
          "a reused index comes back with a new generation");
    check_res(r_close(&R, (uint32_t)h), -EBADF, "  and the retired handle stays EBADF");
    if (h2 > 0)
        check_res(r_close(&R, (uint32_t)h2), 0, "  while the new one closes");

    /* A rejected CLOSE must not consume the handle. */
    h = r_open(&R, PATH_SLOT, HOSTNAME, KORU_O_RDONLY);
    if (h > 0) {
        sqe_close(&s, (uint32_t)h, 0x200);
        s.len = 1;
        check_res(run_one(R.fd, &s), -EINVAL, "CLOSE with a non-zero len is EINVAL");
        sqe_close(&s, (uint32_t)h, 0x201);
        s.off = 1;
        check_res(run_one(R.fd, &s), -EINVAL, "  a non-zero off is EINVAL");
        sqe_close(&s, (uint32_t)h, 0x202);
        s.slot = 1;
        check_res(run_one(R.fd, &s), -EINVAL, "  a non-zero slot is EINVAL");
        check_res(r_close(&R, (uint32_t)h), 0, "  and the handle survived all three");
    }
}

static void path_matrix(void)
{
    struct koru_sqe s;
    uint32_t n, i;

    check_res(r_open(&R, PATH_SLOT, "/no/such/path/here", KORU_O_RDONLY), -ENOENT,
              "a missing path is ENOENT");

    n           = put_path(R.arena, R.slot_size, PATH_SLOT, HOSTNAME);
    R.arena[3]  = 0;
    sqe_open(&s, PATH_SLOT, 0, n, KORU_O_RDONLY, 0x300);
    check_res(run_one(R.fd, &s), -EINVAL, "an embedded NUL is EINVAL");

    n = put_path(R.arena, R.slot_size, PATH_SLOT, HOSTNAME);
    sqe_open(&s, PATH_SLOT, 0, 0, KORU_O_RDONLY, 0x301);
    check_res(run_one(R.fd, &s), -EINVAL, "a zero-length path is EINVAL");
    sqe_open(&s, PATH_SLOT, 0, R.slot_size + 1, KORU_O_RDONLY, 0x302);
    check_res(run_one(R.fd, &s), -EINVAL, "a len past the slot is EINVAL");
    sqe_open(&s, PATH_SLOT, R.slot_size - 4, 8, KORU_O_RDONLY, 0x303);
    check_res(run_one(R.fd, &s), -EINVAL, "an off + len past the slot is EINVAL");
    sqe_open(&s, R.slot_count, 0, n, KORU_O_RDONLY, 0x304);
    check_res(run_one(R.fd, &s), -EINVAL, "a slot past the arena is EINVAL");

    /* "/a/a/a..." resolves nowhere, so only the length clamp separates these. */
    for (i = 0; i < PATH_MAX_; i++)
        R.arena[i] = (i % 2) ? 'a' : '/';
    sqe_open(&s, PATH_SLOT, 0, PATH_MAX_ - 1, KORU_O_RDONLY, 0x305);
    check_res(run_one(R.fd, &s), -ENOENT, "a PATH_MAX-1 path is accepted and resolves");
    sqe_open(&s, PATH_SLOT, 0, PATH_MAX_, KORU_O_RDONLY, 0x306);
    check_res(run_one(R.fd, &s), -EINVAL, "  and a PATH_MAX path is EINVAL");
}

static void flag_matrix(void)
{
    int64_t h;

    check_res(r_open(&R, PATH_SLOT, HOSTNAME, 1u << 8), -EINVAL, "an unknown open flag is EINVAL");
    check_res(r_open(&R, PATH_SLOT, HOSTNAME, KORU_O_ACCMODE), -EINVAL, "access mode 3 is EINVAL");

    h = r_open(&R, PATH_SLOT, "/etc", KORU_O_RDONLY | KORU_O_DIRECTORY);
    check(h > 0, "O_DIRECTORY on a directory succeeds");
    if (h > 0)
        check_res(r_close(&R, (uint32_t)h), 0, "  and closes");
    check_res(r_open(&R, PATH_SLOT, HOSTNAME, KORU_O_RDONLY | KORU_O_DIRECTORY), -ENOTDIR,
              "  O_DIRECTORY on a regular file is ENOTDIR");
    check_res(r_open(&R, PATH_SLOT, "/etc", KORU_O_WRONLY), -EISDIR,
              "O_WRONLY on a directory is EISDIR");

    /* Since T18 OPEN gates on nothing: READ and WRITE carry the file-type
     * rule, and sec_nonblock proves it. */
    h = r_open(&R, PATH_SLOT, "/dev/null", KORU_O_RDONLY);
    check(h > 0, "OPEN of a device node yields a handle");
    if (h > 0)
        check_res(r_close(&R, (uint32_t)h), 0, "  and closes");

    unlink(LINKPATH);
    if (symlink(HOSTNAME, LINKPATH) == 0) {
        h = r_open(&R, PATH_SLOT, LINKPATH, KORU_O_RDONLY);
        check(h > 0, "a symlink is followed by default");
        if (h > 0)
            r_close(&R, (uint32_t)h);
        check_res(r_open(&R, PATH_SLOT, LINKPATH, KORU_O_RDONLY | KORU_O_NOFOLLOW), -ELOOP,
                  "  O_NOFOLLOW on a symlink is ELOOP");
        unlink(LINKPATH);
    } else {
        printf("%-58s SKIP (cannot create a symlink)\n", "O_NOFOLLOW on a symlink is ELOOP");
    }
}

/* Its own ring: a table of eight makes exhaustion cheap to reach. */
static void exhaustion(void)
{
    struct koru_ring m;
    uint32_t held[8];
    int64_t h;
    unsigned i, ok = 0;

    if (ring_open(&m, 32, 64, 8192, 4, 8) != 0 || ring_map(&m) != 0) {
        failures++;
        ring_close(&m);
        return;
    }
    for (i = 0; i < 8; i++) {
        h       = r_open(&m, 0, HOSTNAME, KORU_O_RDONLY);
        held[i] = h > 0 ? (uint32_t)h : 0;
        if (h > 0)
            ok++;
    }
    check(ok == 8, "the whole handle table can be held at once");
    check_res(r_open(&m, 0, HOSTNAME, KORU_O_RDONLY), -EMFILE, "  one more is EMFILE");

    if (held[3]) {
        uint32_t old = held[3];

        check_res(r_close(&m, old), 0, "  freeing one makes room again");
        h = r_open(&m, 0, HOSTNAME, KORU_O_RDONLY);
        check(h > 0 && KORU_HANDLE_INDEX(h) == KORU_HANDLE_INDEX(old) &&
                  KORU_HANDLE_GEN(h) != KORU_HANDLE_GEN(old),
              "  the freed index returns with a new generation");
        check_res(r_close(&m, old), -EBADF, "  and the retired handle is EBADF");
        held[3] = h > 0 ? (uint32_t)h : 0;
    }
    for (i = 0; i < 8; i++)
        if (held[i])
            r_close(&m, held[i]);
    ring_close(&m);
}

/* OPEN reads its path from a slot, so it claims that slot like any other op. */
static void open_slot_test(void)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    const struct koru_cqe *a, *b;
    unsigned completed = 0;
    uint32_t n;
    int ret;

    n = put_path(R.arena, R.slot_size, 1, HOSTNAME);
    sqe_checksum(&sq[0], 1, 0, R.slot_size, 0xa0);
    sqe_open(&sq[1], 1, 0, n, KORU_O_RDONLY, 0xa1);
    ret = submit(R.fd, sq, 2, cq, 2, 2, &completed);
    a   = find_cqe(cq, completed, 0xa0);
    b   = find_cqe(cq, completed, 0xa1);
    check(ret == 2 && completed == 2 && a && b, "a CHECKSUM and an OPEN on one slot both complete");
    if (a && b) {
        check(a->res >= 0, "  the deferred CHECKSUM holds the slot");
        check_res(b->res, -EBUSY, "  and the OPEN behind it gets -EBUSY");
    }
}

void sec_open(void)
{
    handle_matrix();
    path_matrix();
    flag_matrix();
    open_slot_test();
    exhaustion();
}

/* ------------------------------------------------------------------------- */

/* The slot's bytes must equal the pattern starting at file offset `off`. */
static int slot_matches(uint32_t slot, uint64_t off, size_t n)
{
    const uint8_t *p = R.arena + (size_t)slot * R.slot_size;
    size_t i;

    for (i = 0; i < n; i++)
        if (p[i] != pattern_byte(off + i)) {
            note("mismatch at %zu: got %u, want %u", i, p[i], pattern_byte(off + i));
            return 0;
        }
    return 1;
}

void sec_read(void)
{
    uint8_t want[4096];
    int64_t h, stale;
    ssize_t n;
    int f;

    /* 1. The bytes must be what read(2) gives. */
    f = open(HOSTNAME, O_RDONLY);
    n = f >= 0 ? read(f, want, sizeof(want)) : -1;
    if (f >= 0)
        close(f);
    if (n > 0) {
        h = r_open(&R, PATH_SLOT, HOSTNAME, KORU_O_RDONLY);
        if (h > 0) {
            memset(R.arena + R.slot_size, 0, R.slot_size);
            check(r_read(&R, (uint32_t)h, 1, 0, R.slot_size) == n &&
                      memcmp(R.arena + R.slot_size, want, (size_t)n) == 0,
                  "READ of " HOSTNAME " matches read(2) byte for byte");
            check_res(r_close(&R, (uint32_t)h), 0, "  and the handle closes");
        } else {
            check(0, "READ of " HOSTNAME " matches read(2) byte for byte");
        }
    } else {
        printf("%-58s SKIP (no readable " HOSTNAME ")\n", "READ matches read(2)");
    }

    /* 2. The pattern file, every byte predictable. */
    h = r_open(&R, PATH_SLOT, PATFILE, KORU_O_RDONLY);
    check(h > 0, "OPEN the pattern file");
    if (h <= 0)
        return;

    check(r_read(&R, (uint32_t)h, 1, 0, PATSIZE) == PATSIZE && slot_matches(1, 0, PATSIZE),
          "a multi-page READ reproduces the pattern");
    check(r_read(&R, (uint32_t)h, 2, 4097, PATSIZE - 4097) == PATSIZE - 4097 &&
              slot_matches(2, 4097, PATSIZE - 4097),
          "  from an unaligned file offset too");
    check(r_read(&R, (uint32_t)h, 3, PATSIZE - 100, R.slot_size) == 100 &&
              slot_matches(3, PATSIZE - 100, 100),
          "a read running past EOF is short, not an error");
    check_res(r_read(&R, (uint32_t)h, 3, PATSIZE, 64), 0, "  at EOF exactly it is 0");
    check_res(r_read(&R, (uint32_t)h, 3, PATSIZE + 4096, 64), 0, "  past EOF it is 0");
    check(r_read(&R, (uint32_t)h, 4, 123, 1) == 1 && slot_matches(4, 123, 1),
          "a one-byte READ works");

    /* 3. Rejections. */
    check_res(r_read(&R, 0, 1, 0, 64), -EBADF, "READ with handle 0 is EBADF");
    check_res(r_read(&R, (uint32_t)h + (1u << 16), 1, 0, 64), -EBADF,
              "  a stale generation is EBADF");
    check_res(r_read(&R, R.handle_count | (1u << 16), 1, 0, 64), -EBADF,
              "  an index past the table is EBADF");
    check_res(r_read(&R, (uint32_t)h, 1, 0, 0), -EINVAL, "a zero-length READ is EINVAL");
    check_res(r_read(&R, (uint32_t)h, 1, 0, R.slot_size + 1), -EINVAL,
              "  a len past the slot is EINVAL");
    check_res(r_read(&R, (uint32_t)h, R.slot_count, 0, 64), -EINVAL,
              "  a slot past the arena is EINVAL");
    check_res(r_read(&R, (uint32_t)h, 1, (uint64_t)1 << 63, 64), -EINVAL,
              "  a negative file offset is EINVAL");

    /* 4. A CHECKSUM holding the slot refuses the READ behind it. */
    {
        struct koru_sqe sq[2];
        struct koru_cqe cq[2];
        const struct koru_cqe *b;
        unsigned completed = 0;

        sqe_checksum(&sq[0], 5, 0, R.slot_size, 0xa0);
        sqe_read(&sq[1], (uint32_t)h, 5, 0, 64, 0xa1);
        submit(R.fd, sq, 2, cq, 2, 2, &completed);
        b = find_cqe(cq, completed, 0xa1);
        check(completed == 2 && b, "a CHECKSUM and a READ on one slot both complete");
        if (b)
            check_res(b->res, -EBUSY, "  the READ behind it gets -EBUSY");
    }

    stale = h;
    check_res(r_close(&R, (uint32_t)h), 0, "the handle closes");
    check_res(r_read(&R, (uint32_t)stale, 1, 0, 64), -EBADF, "  and a closed handle is EBADF");

    /* 5. Rejected before kernel_read can warn; the script greps for that text. */
    h = r_open(&R, PATH_SLOT, "/etc", KORU_O_RDONLY | KORU_O_DIRECTORY);
    check(h > 0, "OPEN /etc as a directory");
    if (h > 0) {
        check_res(r_read(&R, (uint32_t)h, 1, 0, 64), -EINVAL, "  READ of a directory is EINVAL");
        check_res(r_close(&R, (uint32_t)h), 0, "  and it closes");
    }
}

/* ------------------------------------------------------------------------- */

/* Fill slot `slot` with the pattern for file offset `off`. */
static void fill_slot(uint32_t slot, uint64_t off, size_t n)
{
    uint8_t *p = R.arena + (size_t)slot * R.slot_size;
    size_t i;

    for (i = 0; i < n; i++)
        p[i] = pattern_byte(off + i);
}

/* The file's bytes at `off` must be the pattern for `off`. */
static int file_matches(int fd, uint64_t off, size_t n)
{
    uint8_t buf[8192];
    size_t done = 0;

    while (done < n) {
        size_t want = n - done < sizeof(buf) ? n - done : sizeof(buf);
        ssize_t got = pread(fd, buf, want, (off_t)(off + done));
        size_t i;

        if (got <= 0) {
            note("pread at %llu returned %zd", (unsigned long long)(off + done), got);
            return 0;
        }
        for (i = 0; i < (size_t)got; i++)
            if (buf[i] != pattern_byte(off + done + i)) {
                note("mismatch at %llu", (unsigned long long)(off + done + i));
                return 0;
            }
        done += (size_t)got;
    }
    return 1;
}

void sec_write(void)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    const struct koru_cqe *a, *b;
    unsigned completed = 0;
    int64_t h, ro;
    int fd;

    fd = open(WRFILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        check(0, "create the WRITE target");
        return;
    }

    h = r_open(&R, PATH_SLOT, WRFILE, KORU_O_WRONLY);
    check(h > 0, "OPEN the target for writing");
    if (h <= 0) {
        close(fd);
        unlink(WRFILE);
        return;
    }

    /* 1. A whole slot out, then back through pread(2). */
    fill_slot(1, 0, R.slot_size);
    check_res(r_write(&R, (uint32_t)h, 1, 0, R.slot_size), R.slot_size,
              "a multi-page WRITE reports every byte");
    check(file_matches(fd, 0, R.slot_size), "  and pread(2) reads the pattern back");

    /* 2. Into a hole, at an unaligned offset. Never rewritten, so the file
     *    stays sparse below it. */
    fill_slot(2, 1 << 20, 4096);
    check_res(r_write(&R, (uint32_t)h, 2, 1 << 20, 4096), 4096,
              "a WRITE into a sparse hole lands at its offset");
    check(file_matches(fd, 1 << 20, 4096), "  and reads back there");
    check_res(r_write(&R, (uint32_t)h, 2, 4097, 1), 1, "a one-byte WRITE at an odd offset works");

    /* 3. Rejections. */
    check_res(r_write(&R, 0, 1, 0, 64), -EBADF, "WRITE with handle 0 is EBADF");
    check_res(r_write(&R, (uint32_t)h + (1u << 16), 1, 0, 64), -EBADF,
              "  a stale generation is EBADF");
    check_res(r_write(&R, R.handle_count | (1u << 16), 1, 0, 64), -EBADF,
              "  an index past the table is EBADF");
    check_res(r_write(&R, (uint32_t)h, 1, 0, 0), -EINVAL, "a zero-length WRITE is EINVAL");
    check_res(r_write(&R, (uint32_t)h, 1, 0, R.slot_size + 1), -EINVAL,
              "  a len past the slot is EINVAL");
    check_res(r_write(&R, (uint32_t)h, R.slot_count, 0, 64), -EINVAL,
              "  a slot past the arena is EINVAL");
    check_res(r_write(&R, (uint32_t)h, 1, (uint64_t)1 << 63, 64), -EINVAL,
              "  a negative file offset is EINVAL");

    /* A read-only handle has no FMODE_WRITE. */
    ro = r_open(&R, PATH_SLOT, WRFILE, KORU_O_RDONLY);
    check(ro > 0, "OPEN the same file read-only");
    if (ro > 0) {
        check_res(r_write(&R, (uint32_t)ro, 1, 0, 64), -EBADF, "  WRITE through it is EBADF");
        check_res(r_close(&R, (uint32_t)ro), 0, "  and it closes");
    }

    /* 4. Two WRITEs on one slot: one wins, one gets -EBUSY. */
    fill_slot(3, 0, R.slot_size);
    sqe_write(&sq[0], (uint32_t)h, 3, 0, R.slot_size, 0xb0);
    sqe_write(&sq[1], (uint32_t)h, 3, 0, R.slot_size, 0xb1);
    submit(R.fd, sq, 2, cq, 2, 2, &completed);
    a = find_cqe(cq, completed, 0xb0);
    b = find_cqe(cq, completed, 0xb1);
    check(completed == 2 && a && b, "two WRITEs on one slot both complete");
    if (a && b)
        check(((a->res >= 0) ^ (b->res >= 0)) && ((a->res == -EBUSY) ^ (b->res == -EBUSY)),
              "  exactly one succeeds, the other gets -EBUSY");

    /* 5. A cancelled WRITE must release its slot. Without KORU_OP_WRITE in
     *    OpWork::held_slot this is the only thing that says so. */
    fill_slot(4, 0, R.slot_size);
    sqe_write(&sq[0], (uint32_t)h, 4, 0, R.slot_size, 0x50);
    if (submit(R.fd, sq, 1, cq, 0, 0, &completed) == 1) {
        sqe_cancel(&sq[0], 0x50, 0x51);
        submit(R.fd, sq, 1, cq, 2, 2, &completed);
        check(completed == 2, "a WRITE and its CANCEL both complete");
        sqe_checksum(&sq[0], 4, 0, R.slot_size, 0x52);
        check(run_one(R.fd, &sq[0]) >= 0, "  and the cancelled WRITE released its slot");
    }

    check_res(r_close(&R, (uint32_t)h), 0, "the handle closes");
    close(fd);
    unlink(WRFILE);
}

/* T18: KORU_O_NONBLOCK, and the gate that moved off OPEN onto READ/WRITE. */

#define FIFOPATH "/tmp/koru-check-fifo"

void sec_nonblock(void)
{
    uint8_t buf[64];
    int64_t h, wh;
    uint64_t t0;
    int peer;
    ssize_t n;

    unlink(FIFOPATH);
    if (mkfifo(FIFOPATH, 0600) != 0) {
        printf("%-58s SKIP (cannot mkfifo)\n", "a peerless FIFO opens at once");
        return;
    }

    /* Closes the stall: without the flag filp_open blocks for ever. */
    t0 = now_ms();
    h  = r_open(&R, PATH_SLOT, FIFOPATH, KORU_O_RDONLY | KORU_O_NONBLOCK);
    check(h > 0, "a peerless FIFO opens with KORU_O_NONBLOCK");
    check(now_ms() - t0 < 500, "  and returns at once rather than waiting for a writer");

    /* No writer is EOF, not EAGAIN: pipe_read checks writers first. */
    if (h > 0) {
        check_res(r_read(&R, (uint32_t)h, 1, 0, 64), 0, "  READ with no writer is 0, not EAGAIN");
        check_res(r_close(&R, (uint32_t)h), 0, "  and it closes");
    }

    /* fifo_open owes ENXIO here. */
    check_res(r_open(&R, PATH_SLOT, FIFOPATH, KORU_O_WRONLY | KORU_O_NONBLOCK), -ENXIO,
              "a peerless FIFO opened write-only is ENXIO");

    /* O_RDWR is both peers, so nothing below blocks and no fork is needed. */
    peer = open(FIFOPATH, O_RDWR | O_NONBLOCK);
    if (peer < 0) {
        check(0, "hold the FIFO open as its own peer");
        unlink(FIFOPATH);
        return;
    }

    /* A writer exists and the pipe is empty: now it is EAGAIN. */
    h = r_open(&R, PATH_SLOT, FIFOPATH, KORU_O_RDONLY | KORU_O_NONBLOCK);
    check(h > 0, "OPEN the FIFO non-blocking with a peer attached");
    if (h > 0) {
        check_res(r_read(&R, (uint32_t)h, 1, 0, 64), -EAGAIN, "  an empty FIFO READ is EAGAIN");

        memset(R.arena + R.slot_size, 0, 64);
        check(write(peer, "koru", 4) == 4, "  the peer writes four bytes");
        check(r_read(&R, (uint32_t)h, 1, 0, 64) == 4 &&
                  memcmp(R.arena + R.slot_size, "koru", 4) == 0,
              "  and READ returns them");

        /* No FMODE_LSEEK, so `off` names nothing. */
        check_res(r_read(&R, (uint32_t)h, 1, 1, 64), -EINVAL,
                  "  a non-zero off on an unseekable READ is EINVAL");
        check_res(r_close(&R, (uint32_t)h), 0, "  and it closes");
    }

    /* Delete the gate and this hangs rather than fails. */
    h = r_open(&R, PATH_SLOT, FIFOPATH, KORU_O_RDONLY);
    check(h > 0, "OPEN the FIFO without the flag, which the peer makes possible");
    if (h > 0) {
        check_res(r_read(&R, (uint32_t)h, 1, 0, 64), -EINVAL,
                  "  READ through it is EINVAL, not a blocked kworker");
        check_res(r_close(&R, (uint32_t)h), 0, "  and it closes");
    }

    /* WRITE takes the same gate. */
    wh = r_open(&R, PATH_SLOT, FIFOPATH, KORU_O_WRONLY | KORU_O_NONBLOCK);
    check(wh > 0, "OPEN the FIFO for writing, non-blocking");
    if (wh > 0) {
        memcpy(R.arena + 2 * (size_t)R.slot_size, "ring", 4);
        check_res(r_write(&R, (uint32_t)wh, 2, 0, 4), 4, "  WRITE puts four bytes in");
        n = read(peer, buf, sizeof(buf));
        check(n == 4 && memcmp(buf, "ring", 4) == 0, "  and the peer reads them back");
        check_res(r_write(&R, (uint32_t)wh, 2, 1, 4), -EINVAL,
                  "  a non-zero off on an unseekable WRITE is EINVAL");
        check_res(r_close(&R, (uint32_t)wh), 0, "  and it closes");
    }

    close(peer);
    unlink(FIFOPATH);

    /* Non-blocking clears the type check, so only the f_op guard is left. */
    h = r_open(&R, PATH_SLOT, "/etc", KORU_O_RDONLY | KORU_O_DIRECTORY | KORU_O_NONBLOCK);
    check(h > 0, "OPEN a directory non-blocking");
    if (h > 0) {
        check_res(r_read(&R, (uint32_t)h, 1, 0, 64), -EINVAL,
                  "  READ of it is EINVAL from the f_op guard alone");
        check_res(r_close(&R, (uint32_t)h), 0, "  and it closes");
    }

    /* OPEN no longer gates on the file type; READ does. */
    h = r_open(&R, PATH_SLOT, "/dev/null", KORU_O_RDONLY);
    check(h > 0, "OPEN of a device node now yields a handle");
    if (h > 0) {
        check_res(r_read(&R, (uint32_t)h, 1, 0, 64), -EINVAL,
                  "  but READ without the flag is EINVAL");
        check_res(r_close(&R, (uint32_t)h), 0, "  and it closes");
    }
}

/* T19: a handle for a descriptor the caller already holds. */

/* Child: stdout is the pipe, so it must print nothing. Verdict is the code. */
static int adopt_stdout_child(struct koru_ring *m, int wr)
{
    int64_t h;

    if (ring_map(m) != 0)
        return 2;
    if (dup2(wr, 1) < 0)
        return 3;
    h = r_adopt(m, 1);
    if (h <= 0)
        return 4;
    memcpy(m->arena, "hello", 5);
    if (r_write(m, (uint32_t)h, 0, 0, 5) != 5)
        return 5;
    return r_close(m, (uint32_t)h) == 0 ? 0 : 6;
}

/* Child: the parent opened `fd` on a root-only file before the fork. */
static int adopt_creds_child(struct koru_ring *m, int fd)
{
    struct passwd *pw = getpwnam("nobody");
    uid_t nobody      = pw ? pw->pw_uid : 65534;
    gid_t nogroup     = pw ? pw->pw_gid : 65534;
    int64_t h;

    if (ring_map(m) != 0)
        return 2;
    if (setgroups(0, NULL) != 0 || setresgid(nogroup, nogroup, nogroup) != 0 ||
        setresuid(nobody, nobody, nobody) != 0)
        return 3;
    if (geteuid() == 0)
        return 4;

    /* OPEN would be EACCES here. ADOPT_FD must not be: no permission check. */
    if (r_open(m, 0, SHADOW, KORU_O_RDONLY) != -EACCES)
        return 5;
    h = r_adopt(m, fd);
    if (h <= 0)
        return 6;
    if (r_read(m, (uint32_t)h, 1, 0, 64) < 0)
        return 7;
    return r_close(m, (uint32_t)h) == 0 ? 0 : 8;
}

void sec_adopt(void)
{
    struct koru_sqe s;
    struct koru_ring m;
    int64_t h, stale;
    long before, leaked;
    uint8_t buf[64];
    int fd, pipefd[2];
    pid_t pid;
    int st = 0;
    unsigned i;

    /* 1. Stdout is usable. The pipe is non-blocking because T18's gate needs
     *    it and koru must never set that bit on a file it did not open. */
    if (pipe2(pipefd, O_NONBLOCK) != 0) {
        check(0, "create the stdout pipe");
        return;
    }
    if (ring_open(&m, 32, 64, 4096, 4, 8) != 0) {
        check(0, "a ring for the stdout child");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    pid = fork();
    if (pid == 0)
        _exit(adopt_stdout_child(&m, pipefd[1]));
    close(pipefd[1]);
    if (pid < 0 || waitpid(pid, &st, 0) != pid) {
        check(0, "fork the stdout child");
    } else {
        /* 2 map, 3 dup2, 4 adopt, 5 write, 6 close. */
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a child writes to adopted stdout");
        if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
            note("child verdict %d", WEXITSTATUS(st));
        check(read(pipefd[0], buf, sizeof(buf)) == 5 && memcmp(buf, "hello", 5) == 0,
              "  and the parent reads the bytes off the pipe");
    }
    close(pipefd[0]);
    ring_close(&m);

    /* 2. Adopting any koru fd would make release unreachable. */
    check_res(r_adopt(&R, R.fd), -ELOOP, "adopting our own ring fd is ELOOP");
    if (ring_open(&m, 32, 64, 4096, 4, 8) == 0) {
        check_res(r_adopt(&R, m.fd), -ELOOP, "  and a second ring's fd too");
        ring_close(&m);
    }

    /* 3. Rejections. */
    check_res(r_adopt(&R, 9999), -EBADF, "an out-of-range fd is EBADF");
    fd = open(PATFILE, O_RDONLY);
    check(fd >= 0, "open the pattern file");
    if (fd >= 0) {
        close(fd);
        check_res(r_adopt(&R, fd), -EBADF, "  a closed fd is EBADF");
    }
    sqe_adopt(&s, 0, 0x400);
    s.off = (uint64_t)INT32_MAX + 1;
    check_res(run_one(R.fd, &s), -EINVAL, "an fd past INT32_MAX is EINVAL");
    sqe_adopt(&s, 0, 0x401);
    s.len = 1;
    check_res(run_one(R.fd, &s), -EINVAL, "  a non-zero len is EINVAL");
    sqe_adopt(&s, 0, 0x402);
    s.slot = 1;
    check_res(run_one(R.fd, &s), -EINVAL, "  a non-zero slot is EINVAL");
    sqe_adopt(&s, 0, 0x403);
    s.handle = 1;
    check_res(run_one(R.fd, &s), -EINVAL, "  a non-zero handle is EINVAL");

    /* 4. The reference is ours: closing the descriptor changes nothing. */
    fd = open(PATFILE, O_RDONLY);
    if (fd >= 0) {
        h = r_adopt(&R, fd);
        check(h > 0, "adopt a descriptor on the pattern file");
        close(fd);
        if (h > 0) {
            check(r_read(&R, (uint32_t)h, 1, 0, 64) == 64 && slot_matches(1, 0, 64),
                  "  READ still works after close(2) on the descriptor");
            stale = h;
            check_res(r_close(&R, (uint32_t)h), 0, "  and the handle closes");
            check_res(r_read(&R, (uint32_t)stale, 1, 0, 64), -EBADF, "  after which it is EBADF");
        }
    }

    /* 5. The mirror of OPEN's creds test, asserting the opposite. If this ever
     *    starts failing, something began re-checking permissions at use time. */
    if (geteuid() != 0) {
        printf("%-58s SKIP (not root)\n", "an unprivileged child adopts a root-only fd");
    } else {
        fd = open(SHADOW, O_RDONLY);
        if (fd < 0) {
            printf("%-58s SKIP (cannot open " SHADOW ")\n",
                   "an unprivileged child adopts a root-only fd");
        } else if (ring_open(&m, 32, 64, 8192, 4, 8) != 0) {
            check(0, "a ring for the creds child");
            close(fd);
        } else {
            pid = fork();
            if (pid == 0)
                _exit(adopt_creds_child(&m, fd));
            if (pid < 0 || waitpid(pid, &st, 0) != pid) {
                check(0, "fork the creds child");
            } else {
                /* 5 OPEN not EACCES, 6 adopt failed, 7 read failed. */
                check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                      "an unprivileged child adopts a root-only fd");
                if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
                    note("child verdict %d", WEXITSTATUS(st));
            }
            ring_close(&m);
            close(fd);
        }
    }

    /* 6. Heavy: adopt and close in bulk. filp_open installs no descriptor, but
     *    fget takes a real reference, so file-nr is the instrument. */
    before = file_nr_settled();
    for (i = 0; i < 2000; i++) {
        fd = open(PATFILE, O_RDONLY);
        if (fd < 0)
            break;
        h = r_adopt(&R, fd);
        close(fd);
        if (h > 0)
            r_close(&R, (uint32_t)h);
    }
    check(i == 2000, "2,000 adopt-and-close cycles");
    leaked = file_nr_settled() - before;
    check(leaked < 64, "  and struct file allocations came back");
    if (leaked >= 64)
        note("file-nr leaked %ld", leaked);
}

/* T23: KORU_OP_STAT, whose result goes in the slot rather than in res. */

#define STAT_SLOT  1u
#define STAT_POISON 0xa5
#define STATNS     "/tmp/koru-check-statns"

/* What fstat(2) says the struct must contain. btime is copied through: fstat
 * cannot report it, and `extra` is what says whether it was real. */
static void stat_expect(const struct stat *sb, const struct koru_stat *got,
                        struct koru_stat *want)
{
    memset(want, 0, sizeof(*want));
    want->ino        = sb->st_ino;
    want->size       = (uint64_t)sb->st_size;
    want->blocks     = (uint64_t)sb->st_blocks;
    want->blksize    = (uint64_t)sb->st_blksize;
    want->nlink      = sb->st_nlink;
    want->mode       = sb->st_mode;
    want->uid        = sb->st_uid;
    want->gid        = sb->st_gid;
    want->dev_major  = major(sb->st_dev);
    want->dev_minor  = minor(sb->st_dev);
    want->rdev_major = major(sb->st_rdev);
    want->rdev_minor = minor(sb->st_rdev);
    want->atime_sec  = sb->st_atim.tv_sec;
    want->atime_nsec = (uint64_t)sb->st_atim.tv_nsec;
    want->mtime_sec  = sb->st_mtim.tv_sec;
    want->mtime_nsec = (uint64_t)sb->st_mtim.tv_nsec;
    want->ctime_sec  = sb->st_ctim.tv_sec;
    want->ctime_nsec = (uint64_t)sb->st_ctim.tv_nsec;
    want->btime_sec  = got->btime_sec;
    want->btime_nsec = got->btime_nsec;
}

/* Poison the slot, stat `path` both ways, and require all 256 bytes to match:
 * every byte is a field fstat(2) agrees with, or a zeroed reserved word. A
 * partly filled struct shows up here as poison read back as a kernel value.
 * Returns the CQE's mask, or 0 on failure. */
static uint64_t stat_both_ways(const char *path, const char *what)
{
    struct koru_stat got, want;
    struct stat sb;
    uint8_t *slot  = R.arena + (size_t)STAT_SLOT * R.slot_size;
    uint64_t extra = 0;
    int64_t h, res;
    char label[128];
    int fd, ok;

    fd = open(path, O_RDONLY);
    h  = r_open(&R, PATH_SLOT, path, KORU_O_RDONLY);
    snprintf(label, sizeof(label), "%s: OPEN it both ways", what);
    check(fd >= 0 && h > 0, label);
    if (fd < 0 || h <= 0) {
        if (fd >= 0)
            close(fd);
        return 0;
    }

    memset(slot, STAT_POISON, sizeof(struct koru_stat) + 8);
    res = r_stat(&R, (uint32_t)h, STAT_SLOT, 0, sizeof(struct koru_stat), &extra);
    ok  = fstat(fd, &sb) == 0;
    close(fd);
    r_close(&R, (uint32_t)h);

    snprintf(label, sizeof(label), "  res is the whole struct, and fstat(2) agrees");
    if (res != (int64_t)sizeof(struct koru_stat) || !ok) {
        check(0, label);
        note("res %lld, fstat %d", (long long)res, ok);
        return 0;
    }
    memcpy(&got, slot, sizeof(got));
    stat_expect(&sb, &got, &want);
    check(memcmp(&got, &want, sizeof(got)) == 0, label);
    if (memcmp(&got, &want, sizeof(got)) != 0) {
        note("ino %llu/%llu size %llu/%llu mode %llo/%llo uid %llu/%llu",
             (unsigned long long)got.ino, (unsigned long long)want.ino,
             (unsigned long long)got.size, (unsigned long long)want.size,
             (unsigned long long)got.mode, (unsigned long long)want.mode,
             (unsigned long long)got.uid, (unsigned long long)want.uid);
    }

    /* The sentinel past the struct: res says where the kernel stopped. */
    snprintf(label, sizeof(label), "  and it wrote not one byte past res");
    check(slot[sizeof(struct koru_stat)] == STAT_POISON, label);
    return extra;
}

static void stat_rejections(int64_t h)
{
    struct koru_sqe s;

    sqe_stat(&s, (uint32_t)h, STAT_SLOT, 4, sizeof(struct koru_stat), 0x600);
    check_res(run_one(R.fd, &s), -EINVAL, "an unaligned off is EINVAL");
    check_res(r_stat(&R, (uint32_t)h, STAT_SLOT, 0, 0, NULL), -EINVAL,
              "a zero len is EINVAL");
    check_res(r_stat(&R, (uint32_t)h, STAT_SLOT, R.slot_size - 8, 16, NULL), -EINVAL,
              "an off + len past the slot is EINVAL");
    check_res(r_stat(&R, (uint32_t)h, STAT_SLOT, UINT64_MAX & ~7ull, 16, NULL), -EINVAL,
              "an off + len that overflows is EINVAL");
    check_res(r_stat(&R, (uint32_t)h, R.slot_count, 0, 64, NULL), -EINVAL,
              "a slot past the arena is EINVAL");
    check_res(r_stat(&R, 0, STAT_SLOT, 0, 64, NULL), -EBADF, "handle 0 is EBADF");
    check_res(r_stat(&R, (uint32_t)h + (1u << 16), STAT_SLOT, 0, 64, NULL), -EBADF,
              "  a stale generation is EBADF");
    check_res(r_stat(&R, R.handle_count | (1u << 16), STAT_SLOT, 0, 64, NULL), -EBADF,
              "  an index past the table is EBADF");
}

static int write_file(const char *path, const char *text)
{
    ssize_t n = (ssize_t)strlen(text);
    int fd    = open(path, O_WRONLY);

    if (fd < 0)
        return -1;
    if (write(fd, text, (size_t)n) != n) {
        close(fd);
        return -1;
    }
    return close(fd);
}

/* In its own user namespace, root's files must stat as the shifted id, and
 * koru must agree with fstat(2). The kernel translates in a kworker, where
 * current_user_ns() is init's — so this is what proves OpWork carries the
 * submitter's creds rather than the worker's. Delete that and every uid below
 * comes back unshifted. */
static int stat_ns_child(struct koru_ring *m)
{
    struct koru_stat ks;
    struct stat sb;
    int64_t h, res;
    int fd;

    if (ring_map(m) != 0)
        return 2;
    if (unshare(CLONE_NEWUSER) != 0)
        return 3;
    /* gid_map needs this unless we hold CAP_SETGID in the parent namespace,
     * which unshare has just taken away. uid_map needs no equivalent: the
     * single mapped id is our own. */
    if (write_file("/proc/self/setgroups", "deny") != 0)
        return 4;
    if (write_file("/proc/self/uid_map", "100 0 1\n") != 0)
        return 5;
    if (write_file("/proc/self/gid_map", "200 0 1\n") != 0)
        return 6;

    fd = open(STATNS, O_RDONLY);
    h  = r_open(m, 0, STATNS, KORU_O_RDONLY);
    if (fd < 0 || h <= 0)
        return 7;
    res = r_stat(m, (uint32_t)h, 1, 0, sizeof(ks), NULL);
    if (fstat(fd, &sb) != 0 || res != (int64_t)sizeof(ks))
        return 8;
    memcpy(&ks, m->arena + m->slot_size, sizeof(ks));
    close(fd);
    r_close(m, (uint32_t)h);

    /* Shifted, and the same shift fstat(2) reports. */
    if (sb.st_uid != 100 || sb.st_gid != 200)
        return 9;
    if (ks.uid != sb.st_uid || ks.gid != sb.st_gid)
        return 10;
    return 0;
}

static void stat_namespace(void)
{
    struct koru_ring m;
    pid_t pid;
    int st = 0, fd;

    if (geteuid() != 0) {
        printf("%-58s SKIP (not root)\n", "a stat in a user namespace reports the shifted uid");
        return;
    }
    fd = open(STATNS, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || fchown(fd, 0, 0) != 0) {
        printf("%-58s SKIP (cannot create " STATNS ")\n",
               "a stat in a user namespace reports the shifted uid");
        if (fd >= 0)
            close(fd);
        return;
    }
    close(fd);

    /* VM_DONTCOPY: the parent leaves this ring unmapped, the child maps it. */
    if (ring_open(&m, 32, 64, 8192, 4, 8) != 0) {
        check(0, "a ring for the namespace child");
        unlink(STATNS);
        return;
    }
    pid = fork();
    if (pid == 0)
        _exit(stat_ns_child(&m));
    if (pid < 0 || waitpid(pid, &st, 0) != pid) {
        check(0, "fork the namespace child");
    } else if (WIFEXITED(st) && WEXITSTATUS(st) == 3) {
        printf("%-58s SKIP (no CONFIG_USER_NS)\n",
               "a stat in a user namespace reports the shifted uid");
    } else {
        /* 4-6 map writes, 7 open, 8 stat, 9 fstat unshifted, 10 disagreed. */
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "a stat in a user namespace reports the shifted uid");
        if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
            note("child verdict %d", WEXITSTATUS(st));
    }
    ring_close(&m);
    unlink(STATNS);
}

void sec_stat(void)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    const struct koru_cqe *a, *b;
    uint8_t *slot = R.arena + (size_t)STAT_SLOT * R.slot_size;
    unsigned completed = 0;
    uint64_t extra;
    int64_t h, res;

    /* 1. Every field, against fstat(2), for three shapes of file. */
    extra = stat_both_ways(PATFILE, "a regular file");
    check((extra & ~(uint64_t)KORU_STAT_ALL) == 0, "  extra carries no bit outside KORU_STAT_ALL");
    check((extra & (KORU_STAT_ALL & ~KORU_STAT_BTIME)) == (KORU_STAT_ALL & ~KORU_STAT_BTIME),
          "  and every field but btime was reported");
    check((extra & KORU_STAT_BTIME) != 0, "  tmpfs reports a creation time too");
    /* The mask is koru's own, so the statx one must not read as valid here. */
    check(extra != 4095, "  and it is not the statx mask passed through");

    stat_both_ways("/etc", "a directory");
    stat_both_ways("/dev/null", "a character device");

    /* 2. len is the caller's buffer size, and its version negotiation. */
    h = r_open(&R, PATH_SLOT, PATFILE, KORU_O_RDONLY);
    check(h > 0, "OPEN the pattern file");
    if (h <= 0)
        return;

    memset(slot, STAT_POISON, sizeof(struct koru_stat) + 8);
    check_res(r_stat(&R, (uint32_t)h, STAT_SLOT, 0, 64, NULL), 64,
              "a len short of the struct returns exactly that len");
    check(slot[64] == STAT_POISON, "  and leaves the byte after it untouched");
    check(slot[0] != STAT_POISON, "  while the prefix really was written");

    memset(slot, STAT_POISON, sizeof(struct koru_stat) + 8);
    check_res(r_stat(&R, (uint32_t)h, STAT_SLOT, 0, 4096, NULL), sizeof(struct koru_stat),
              "a len past the struct is clamped to the struct");
    check(slot[sizeof(struct koru_stat)] == STAT_POISON, "  and nothing beyond it is touched");

    /* 3. A destination straddling a page boundary, which write_slot must split. */
    {
        struct koru_stat got, want;
        struct stat sb;
        int fd = open(PATFILE, O_RDONLY);

        memset(slot + 3968, STAT_POISON, sizeof(struct koru_stat) + 8);
        res = r_stat(&R, (uint32_t)h, STAT_SLOT, 3968, sizeof(struct koru_stat), NULL);
        check(fd >= 0 && fstat(fd, &sb) == 0 && res == (int64_t)sizeof(struct koru_stat),
              "a stat at slot offset 3968 crosses a page boundary");
        if (fd >= 0)
            close(fd);
        memcpy(&got, slot + 3968, sizeof(got));
        stat_expect(&sb, &got, &want);
        check(memcmp(&got, &want, sizeof(got)) == 0, "  and lands intact on both pages");
    }

    /* 4. Rejections. */
    stat_rejections(h);

    /* 5. Slot exclusivity. A whole-slot CHECKSUM is slow enough to still hold
     *    the slot when the STAT behind it is dispatched. */
    sqe_checksum(&sq[0], STAT_SLOT, 0, R.slot_size, 0xd0);
    sqe_stat(&sq[1], (uint32_t)h, STAT_SLOT, 0, sizeof(struct koru_stat), 0xd1);
    submit(R.fd, sq, 2, cq, 2, 2, &completed);
    a = find_cqe(cq, completed, 0xd0);
    b = find_cqe(cq, completed, 0xd1);
    check(completed == 2 && a && b, "a CHECKSUM and a STAT on one slot both complete");
    if (a && b) {
        check(a->res >= 0, "  the deferred CHECKSUM holds the slot");
        check_res(b->res, -EBUSY, "  and the STAT behind it gets -EBUSY");
        check(b->extra == 0, "  a refused STAT reports no mask");
    }

    /* 6. A cancelled STAT must release its slot: KORU_OP_STAT in held_slot is
     *    the only thing that does it, and nothing else says so. */
    sqe_stat(&sq[0], (uint32_t)h, STAT_SLOT, 0, sizeof(struct koru_stat), 0x70);
    if (submit(R.fd, sq, 1, cq, 0, 0, &completed) == 1) {
        sqe_cancel(&sq[0], 0x70, 0x71);
        submit(R.fd, sq, 1, cq, 2, 2, &completed);
        check(completed == 2, "a STAT and its CANCEL both complete");
        sqe_checksum(&sq[0], STAT_SLOT, 0, R.slot_size, 0x72);
        check(run_one(R.fd, &sq[0]) >= 0, "  and the cancelled STAT released its slot");
    }

    check_res(r_close(&R, (uint32_t)h), 0, "the handle closes");

    /* 7. The ids are the submitter's, not the kworker's. */
    stat_namespace();
}

/* T24: TRUNCATE, UTIMES and READLINK, the kern_path plumbing. */

#define TRFILE   "/tmp/koru-check-trunc"
#define TRLINK   "/tmp/koru-check-trunclink"
#define PLINK    "/tmp/koru-check-plink"
#define PLINK2   "/tmp/koru-check-plink2"
#define LONGLINK "/run/koru-check-longlink"
/* Exactly eight characters, so a path op can name it from a slot offset that
 * leaves no room for the argument after it. */
#define SHORTPATH "/run/abc"
#define CREDSDIR  "/tmp/koru-check-credsdir"
#define CREDSLINK CREDSDIR "/link"
/* Longer than tmpfs's SHORT_SYMLINK_LEN, so the target is page-backed and
 * vfs_get_link arms a delayed call. A short one arms none and leaks nothing. */
#define LONGTARGET                                                                                 \
    "/tmp/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb/"                      \
    "cccccccccccccccccccccccccccccccc/dddddddddddddddddddddddddddddddd/target"

static int64_t r_truncate(struct koru_ring *r, uint32_t slot, const char *path, uint64_t size)
{
    return r_path(r, KORU_OP_TRUNCATE, slot, path, &size, sizeof(size));
}

static int64_t r_utimes(struct koru_ring *r, uint32_t slot, const char *path,
                        const struct koru_times *t)
{
    return r_path(r, KORU_OP_UTIMES, slot, path, t, sizeof(*t));
}

static int64_t r_readlink(struct koru_ring *r, uint32_t slot, const char *path)
{
    return r_path(r, KORU_OP_READLINK, slot, path, NULL, 0);
}

/* st_size of `path`, or -1. */
static int64_t path_size(const char *path)
{
    struct stat sb;

    return stat(path, &sb) == 0 ? (int64_t)sb.st_size : -1;
}

static int make_file(const char *path, size_t n)
{
    uint8_t buf[4096];
    size_t done = 0;
    int fd      = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        return -1;
    memset(buf, 0x5a, sizeof(buf));
    while (done < n) {
        size_t want = n - done < sizeof(buf) ? n - done : sizeof(buf);

        if (write(fd, buf, want) != (ssize_t)want) {
            close(fd);
            return -1;
        }
        done += want;
    }
    return close(fd);
}

static void path_truncate(void)
{
    struct koru_sqe s;
    uint64_t size;

    if (make_file(TRFILE, 8192) != 0) {
        check(0, "create the TRUNCATE target");
        return;
    }

    check_res(r_truncate(&R, 1, TRFILE, 100), 0, "TRUNCATE shrinks a file");
    check(path_size(TRFILE) == 100, "  and stat(2) agrees");
    check_res(r_truncate(&R, 1, TRFILE, 1 << 20), 0, "  it extends one too");
    check(path_size(TRFILE) == (1 << 20), "  and stat(2) agrees again");
    check_res(r_truncate(&R, 1, TRFILE, 0), 0, "  and truncates to nothing");
    check(path_size(TRFILE) == 0, "  leaving an empty file");

    /* A final symlink is followed, as truncate(2) follows it. */
    unlink(TRLINK);
    if (symlink(TRFILE, TRLINK) == 0) {
        check_res(r_truncate(&R, 1, TRLINK, 4096), 0, "TRUNCATE follows a final symlink");
        check(path_size(TRFILE) == 4096, "  and the target is what changed");
        unlink(TRLINK);
    } else {
        printf("%-58s SKIP (cannot symlink)\n", "TRUNCATE follows a final symlink");
    }

    /* Rejections. vfs_truncate owns the first two. */
    check_res(r_truncate(&R, 1, "/etc", 0), -EISDIR, "TRUNCATE of a directory is EISDIR");
    check_res(r_truncate(&R, 1, "/dev/null", 0), -EINVAL, "  of a device node is EINVAL");
    check_res(r_truncate(&R, 1, "/no/such/path", 0), -ENOENT, "  of a missing path is ENOENT");
    check_res(r_truncate(&R, 1, TRFILE, (uint64_t)1 << 63), -EINVAL,
              "  a negative new length is EINVAL");

    size = 0;
    memcpy(R.arena + R.slot_size + arg_offset(0, 8), &size, sizeof(size));
    sqe_path(&s, KORU_OP_TRUNCATE, 1, 0, 0, 0x700);
    check_res(run_one(R.fd, &s), -EINVAL, "  a zero-length path is EINVAL");
    put_path(R.arena, R.slot_size, 1, TRFILE);
    sqe_path(&s, KORU_OP_TRUNCATE, 1, 0, (uint32_t)strlen(TRFILE), 0x701);
    s.handle = 1;
    check_res(run_one(R.fd, &s), -EINVAL, "  a non-zero handle is EINVAL");
    sqe_path(&s, KORU_OP_TRUNCATE, R.slot_count, 0, 8, 0x702);
    check_res(run_one(R.fd, &s), -EINVAL, "  a slot past the arena is EINVAL");

    /* The argument has to fit after the path, not merely the path itself. The
     * path here is real and in range, so only the argument bound can refuse. */
    if (make_file(SHORTPATH, 0) == 0) {
        memcpy(R.arena + R.slot_size + R.slot_size - 12, SHORTPATH, 8);
        sqe_path(&s, KORU_OP_TRUNCATE, 1, R.slot_size - 12, 8, 0x703);
        check_res(run_one(R.fd, &s), -EINVAL, "  an argument past the slot end is EINVAL");
        unlink(SHORTPATH);
    } else {
        check(0, "  an argument past the slot end is EINVAL");
    }

    unlink(TRFILE);
}

static void path_utimes(void)
{
    struct koru_times t;
    struct stat sb;
    time_t before;

    if (make_file(TRFILE, 64) != 0) {
        check(0, "create the UTIMES target");
        return;
    }

    memset(&t, 0, sizeof(t));
    t.atime_sec  = 1000000000;
    t.atime_nsec = 123456789;
    t.mtime_sec  = 1100000000;
    t.mtime_nsec = 987654321;
    check_res(r_utimes(&R, 1, TRFILE, &t), 0, "UTIMES sets both timestamps");
    if (stat(TRFILE, &sb) == 0) {
        check(sb.st_atim.tv_sec == t.atime_sec && sb.st_atim.tv_nsec == t.atime_nsec,
              "  stat(2) reports the exact atime");
        check(sb.st_mtim.tv_sec == t.mtime_sec && sb.st_mtim.tv_nsec == t.mtime_nsec,
              "  and the exact mtime");
    } else {
        check(0, "  stat(2) reports the exact atime");
    }

    /* OMIT leaves one alone; the kernel's own sentinel, checked by vfs_utimes. */
    t.atime_nsec = KORU_UTIME_OMIT;
    t.mtime_sec  = 1200000000;
    t.mtime_nsec = 1;
    check_res(r_utimes(&R, 1, TRFILE, &t), 0, "UTIME_OMIT is accepted");
    if (stat(TRFILE, &sb) == 0) {
        check(sb.st_atim.tv_sec == 1000000000 && sb.st_atim.tv_nsec == 123456789,
              "  and the atime is untouched");
        check(sb.st_mtim.tv_sec == 1200000000 && sb.st_mtim.tv_nsec == 1,
              "  while the mtime moved");
    } else {
        check(0, "  and the atime is untouched");
    }

    before       = time(NULL);
    t.atime_nsec = KORU_UTIME_NOW;
    t.mtime_nsec = KORU_UTIME_OMIT;
    check_res(r_utimes(&R, 1, TRFILE, &t), 0, "UTIME_NOW is accepted");
    if (stat(TRFILE, &sb) == 0) {
        check(sb.st_atim.tv_sec >= before && sb.st_atim.tv_sec <= before + 5,
              "  and the atime is now");
        check(sb.st_mtim.tv_sec == 1200000000, "  while the omitted mtime stayed");
    } else {
        check(0, "  and the atime is now");
    }

    /* vfs_utimes validates the nanoseconds itself. */
    t.atime_nsec = 1000000000;
    t.mtime_nsec = 0;
    check_res(r_utimes(&R, 1, TRFILE, &t), -EINVAL, "a nanosecond field past 999999999 is EINVAL");
    t.atime_nsec = -1;
    check_res(r_utimes(&R, 1, TRFILE, &t), -EINVAL, "  and a negative one too");

    t.atime_nsec = 0;
    check_res(r_utimes(&R, 1, "/no/such/path", &t), -ENOENT, "UTIMES of a missing path is ENOENT");

    unlink(TRFILE);
}

/* The slot's bytes at `off` must be `want` followed by a NUL. */
static int link_matches(uint32_t slot, uint64_t off, const char *want)
{
    const char *p = (const char *)R.arena + (size_t)slot * R.slot_size + off;
    size_t n      = strlen(want);

    return memcmp(p, want, n) == 0 && p[n] == 0;
}

static void path_readlink(void)
{
    struct koru_sqe s;
    char want[512];
    ssize_t n;
    uint32_t plen;

    unlink(PLINK);
    unlink(PLINK2);
    if (symlink(PLINK2, PLINK) != 0 || symlink("/etc/hostname", PLINK2) != 0) {
        printf("%-58s SKIP (cannot symlink)\n", "READLINK reproduces readlink(2)");
        return;
    }

    n = readlink(PLINK, want, sizeof(want) - 1);
    check(n > 0, "readlink(2) reads the first link");
    if (n > 0) {
        want[n] = 0;
        check_res(r_readlink(&R, 1, PLINK), n, "READLINK returns the length without the NUL");
        check(link_matches(1, 0, want), "  and the slot holds what readlink(2) gave");
        /* PLINK points at PLINK2, which points at a file. Following would give
         * neither of these answers. */
        check(strcmp(want, PLINK2) == 0, "  which is the first target, not the last");
    }

    check_res(r_readlink(&R, 1, "/etc/hostname"), -EINVAL, "READLINK of a regular file is EINVAL");
    check_res(r_readlink(&R, 1, "/etc"), -EINVAL, "  of a directory is EINVAL");
    check_res(r_readlink(&R, 1, "/no/such/path"), -ENOENT, "  of a missing path is ENOENT");

    /* The answer replaces the path, so the room is the rest of the slot.
     * Truncating silently is how a wrong path gets used. */
    unlink(LONGLINK);
    if (symlink(LONGTARGET, LONGLINK) == 0) {
        check_res(r_readlink(&R, 1, LONGLINK), (int64_t)strlen(LONGTARGET),
                  "READLINK of a page-backed symlink works");
        check(link_matches(1, 0, LONGTARGET), "  and reproduces the long target");

        plen = put_path(R.arena, R.slot_size, 1, LONGLINK);
        memmove(R.arena + R.slot_size + R.slot_size - 64, R.arena + R.slot_size, plen);
        sqe_path(&s, KORU_OP_READLINK, 1, R.slot_size - 64, plen, 0x710);
        check_res(run_one(R.fd, &s), -ENAMETOOLONG, "  and a target that does not fit is refused");
        unlink(LONGLINK);
    } else {
        printf("%-58s SKIP (cannot symlink)\n", "READLINK of a page-backed symlink works");
    }

    put_path(R.arena, R.slot_size, 1, PLINK);
    sqe_path(&s, KORU_OP_READLINK, 1, 0, (uint32_t)strlen(PLINK), 0x711);
    s.handle = 1;
    check_res(run_one(R.fd, &s), -EINVAL, "READLINK with a non-zero handle is EINVAL");

    unlink(PLINK);
    unlink(PLINK2);
}

/* The whole inline-because-of-creds rule, for something other than OPEN.
 * Deferred to a kworker every one of these would run as root in the initial
 * namespaces, and each EACCES below would become a success. */
static int path_creds_child(struct koru_ring *m)
{
    struct passwd *pw = getpwnam("nobody");
    uid_t nobody      = pw ? pw->pw_uid : 65534;
    gid_t nogroup     = pw ? pw->pw_gid : 65534;
    struct koru_times t;

    if (ring_map(m) != 0)
        return 2;
    if (setgroups(0, NULL) != 0 || setresgid(nogroup, nogroup, nogroup) != 0 ||
        setresuid(nobody, nobody, nobody) != 0)
        return 3;
    if (geteuid() == 0)
        return 4;

    if (r_truncate(m, 0, TRFILE, 0) != -EACCES)
        return 5;
    /* Explicit times need ownership, so setattr_prepare gives EPERM. */
    memset(&t, 0, sizeof(t));
    t.atime_sec = 1;
    t.mtime_sec = 1;
    if (r_utimes(m, 0, TRFILE, &t) != -EPERM)
        return 6;
    /* Both UTIME_NOW is a touch, which needs only write: EACCES instead. */
    t.atime_nsec = KORU_UTIME_NOW;
    t.mtime_nsec = KORU_UTIME_NOW;
    if (r_utimes(m, 0, TRFILE, &t) != -EACCES)
        return 7;
    /* The link itself is world-readable; the directory it sits in is not. */
    if (r_readlink(m, 0, CREDSLINK) != -EACCES)
        return 8;
    return 0;
}

static void path_creds(void)
{
    struct koru_ring m;
    pid_t pid;
    int st = 0;

    if (geteuid() != 0) {
        printf("%-58s SKIP (not root)\n", "an unprivileged TRUNCATE is EACCES");
        return;
    }
    if (make_file(TRFILE, 64) != 0 || chmod(TRFILE, 0600) != 0) {
        check(0, "create the root-owned target");
        return;
    }
    unlink(CREDSLINK);
    rmdir(CREDSDIR);
    if (mkdir(CREDSDIR, 0700) != 0 || symlink("/etc/hostname", CREDSLINK) != 0) {
        check(0, "create the root-only directory");
        unlink(TRFILE);
        return;
    }

    /* VM_DONTCOPY: the parent leaves this ring unmapped, the child maps it. */
    if (ring_open(&m, 32, 64, 8192, 4, 8) != 0) {
        check(0, "a ring for the path creds child");
    } else {
        pid = fork();
        if (pid == 0)
            _exit(path_creds_child(&m));
        if (pid < 0 || waitpid(pid, &st, 0) != pid) {
            check(0, "fork the path creds child");
        } else {
            /* 5 truncate, 6 utimes, 7 touch, 8 readlink. */
            check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "an unprivileged TRUNCATE, UTIMES and READLINK are refused");
            if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
                note("child verdict %d", WEXITSTATUS(st));
        }
        ring_close(&m);
    }
    unlink(CREDSLINK);
    rmdir(CREDSDIR);
    unlink(TRFILE);
}

/* Heavy: a page-backed symlink's get_link takes a folio reference and the
 * delayed call is the only thing that puts it back. kmemleak cannot see that —
 * the page is still referenced, just for ever — so the instrument is MemFree,
 * with each link unlinked so its page would otherwise be freed. */
#define LEAKLINKS 4000u

static void path_readlink_leak(void)
{
    char path[64];
    long before, after;
    unsigned i, made = 0;
    int ok = 1;

    before = mem_free_kb();
    for (i = 0; i < LEAKLINKS; i++) {
        snprintf(path, sizeof(path), "%s-%u", LONGLINK, i);
        unlink(path);
        if (symlink(LONGTARGET, path) != 0)
            break;
        made++;
        if (r_readlink(&R, 1, path) != (int64_t)strlen(LONGTARGET))
            ok = 0;
        unlink(path);
    }
    check(made == LEAKLINKS && ok, "4,000 readlinks of page-backed symlinks");

    /* Each leak is one page; the loop's own churn is the noise floor. */
    after = mem_free_kb();
    note("MemFree %ld -> %ld kB (%ld)", before, after, before - after);
    check(before > 0 && after > 0 && before - after < (long)LEAKLINKS * 2,
          "  and every target page came back");
}

void sec_path(void)
{
    path_truncate();
    path_utimes();
    path_readlink();
    path_creds();
    path_readlink_leak();
}

void sec_delay(void)
{
    struct koru_sqe sq[8];
    struct koru_cqe cq[8];
    struct koru_enter e;
    struct koru_params p;
    uint64_t t0, dt;
    unsigned i, completed = 0;
    int ret, ok;

    /* 1. Concurrent, not serial. */
    for (i = 0; i < 4; i++)
        sqe_delay(&sq[i], 0x1000 + i, 50 * MS);
    enter_init(&e, sq, 4, cq, 8);
    e.min_complete = 4;
    t0             = now_ms();
    ret            = ioctl(R.fd, KORU_IOC_ENTER, &e);
    dt             = now_ms() - t0;
    check(ret == 4 && e.submitted == 4 && e.completed == 4, "four 50 ms delays all complete");
    check(dt >= 40, "  ENTER waited for them");
    check(dt < 150, "  and they ran concurrently, not serially");
    note("four 50 ms delays took %llu ms", (unsigned long long)dt);
    ok = 1;
    for (i = 0; i < 4; i++)
        if (cq[i].res != 0 || !find_cqe(cq, 4, 0x1000 + i))
            ok = 0;
    check(ok, "  each completes res 0, and every user_data comes back");

    /* 2. timeout_ns caps the wait; min_complete decides whether to wait. */
    sqe_delay(&sq[0], 0x2000, 2000 * MS);
    enter_init(&e, sq, 1, cq, 8);
    e.min_complete = 1;
    e.timeout_ns   = 10 * MS;
    t0             = now_ms();
    ret            = ioctl(R.fd, KORU_IOC_ENTER, &e);
    dt             = now_ms() - t0;
    check(ret == 1 && e.completed == 0, "a 10 ms timeout against a 2 s delay reaps nothing");
    check(dt < 500, "  and returns without waiting the delay out");

    enter_init(&e, NULL, 0, cq, 8);
    t0  = now_ms();
    ret = ioctl(R.fd, KORU_IOC_ENTER, &e);
    dt  = now_ms() - t0;
    check(ret == 0 && dt < 200, "min_complete 0 returns at once with work in flight");

    sqe_cancel(&sq[0], 0x2000, 0x2001);
    check(submit(R.fd, sq, 1, cq, 8, 2, &completed) == 1 && completed == 2,
          "  and the delay can be cancelled rather than waited out");

    /* 3. Unreachable means inflight == 0, not len == 0 && inflight == 0. */
    sqe_nop(&sq[0], 0x3000);
    t0  = now_ms();
    ret = submit(R.fd, sq, 1, cq, 8, 3, &completed);
    check(ret == 1 && completed == 1, "min_complete past what can arrive returns short");
    check(now_ms() - t0 < 500, "  rather than sleeping for ever");

    enter_init(&e, NULL, 0, cq, 8);
    e.min_complete = 1;
    t0             = now_ms();
    ret            = ioctl(R.fd, KORU_IOC_ENTER, &e);
    dt             = now_ms() - t0;
    check(ret == 0 && e.completed == 0 && dt < 200, "  and an idle ring returns at once");

    /* 4. DELAY_NS reads only off. */
    sqe_delay(&sq[0], 0x4000, MS);
    sq[0].len = 1;
    check_res(run_one(R.fd, &sq[0]), -EINVAL, "DELAY_NS with a non-zero len is EINVAL");
    sqe_delay(&sq[0], 0x4001, MS);
    sq[0].handle = 9;
    check_res(run_one(R.fd, &sq[0]), -EINVAL, "  a non-zero handle is EINVAL");
    sqe_delay(&sq[0], 0x4002, MS);
    sq[0].slot = 1;
    check_res(run_one(R.fd, &sq[0]), -EINVAL, "  a non-zero slot is EINVAL");

    /* 5. Both sides of the cap; the accepted one can only be cancelled. */
    memset(&p, 0, sizeof(p));
    if (ioctl(R.fd, KORU_IOC_GET_PARAMS, &p) == 0 && p.max_delay_ns > 0) {
        sqe_delay(&sq[0], 0x5000, p.max_delay_ns);
        check(submit(R.fd, sq, 1, cq, 0, 0, &completed) == 1,
              "a delay exactly at the cap is accepted");
        sqe_cancel(&sq[0], 0x5000, 0x5001);
        submit(R.fd, sq, 1, cq, 8, 2, &completed);
        check(completed == 2, "  and both it and its cancel complete");
        sqe_delay(&sq[0], 0x5002, p.max_delay_ns + 1);
        check_res(run_one(R.fd, &sq[0]), -EINVAL, "  one nanosecond past the cap is EINVAL");
    } else {
        check(0, "GET_PARAMS reports max_delay_ns");
    }
}

/* Only the elapsed time distinguishes a real dequeue: the res values are the
 * same either way. */

void sec_cancel(void)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    const struct koru_cqe *t, *c;
    unsigned completed = 0;
    uint64_t t0, elapsed;
    int ret;

    /* cq_space 0, so the cancel's own ENTER reaps both completions. */
    sqe_delay(&sq[0], 0x10, 2000 * MS);
    check(submit(R.fd, sq, 1, cq, 0, 0, &completed) == 1 && completed == 0,
          "a 2 s DELAY_NS is queued");

    t0 = now_ms();
    sqe_cancel(&sq[0], 0x10, 0x11);
    ret     = submit(R.fd, sq, 1, cq, 4, 2, &completed);
    elapsed = now_ms() - t0;

    check(ret == 1 && completed == 2, "the CANCEL and its target both complete (C1)");
    t = find_cqe(cq, completed, 0x10);
    c = find_cqe(cq, completed, 0x11);
    if (t && c) {
        check_res(c->res, 0, "  the canceller gets 0");
        check_res(t->res, -ECANCELED, "  the target gets -ECANCELED");
    } else {
        check(0, "  both CQEs carry their own user_data");
    }
    check(elapsed < 500, "  and it returned at once, not after two seconds");
    if (elapsed >= 500)
        note("elapsed %llu ms", (unsigned long long)elapsed);

    /* Rejections. */
    sqe_cancel(&sq[0], 0xdeadbeef, 0x20);
    check_res(run_one(R.fd, &sq[0]), -ENOENT, "CANCEL of an unknown user_data is ENOENT");
    sqe_nop(&sq[0], 0x21);
    check_res(run_one(R.fd, &sq[0]), 0, "a NOP completes");
    sqe_cancel(&sq[0], 0x21, 0x22);
    check_res(run_one(R.fd, &sq[0]), -ENOENT, "  CANCEL of a completed op is ENOENT");
    sqe_cancel(&sq[0], 0x10, 0x23);
    check_res(run_one(R.fd, &sq[0]), -ENOENT, "a second CANCEL of a cancelled op is ENOENT");

    sqe_cancel(&sq[0], 1, 0x24);
    sq[0].len = 1;
    check_res(run_one(R.fd, &sq[0]), -EINVAL, "CANCEL with a non-zero len is EINVAL");
    sqe_cancel(&sq[0], 1, 0x25);
    sq[0].slot = 1;
    check_res(run_one(R.fd, &sq[0]), -EINVAL, "  a non-zero slot is EINVAL");
    sqe_cancel(&sq[0], 1, 0x26);
    sq[0].handle = 1;
    check_res(run_one(R.fd, &sq[0]), -EINVAL, "  a non-zero handle is EINVAL");

    /* A cancelled op must release its slot. */
    memset(R.arena + 2 * (size_t)R.slot_size, 0xa5, R.slot_size);
    sqe_checksum(&sq[0], 2, 0, R.slot_size, 0x40);
    if (submit(R.fd, sq, 1, cq, 0, 0, &completed) == 1) {
        sqe_cancel(&sq[0], 0x40, 0x41);
        submit(R.fd, sq, 1, cq, 4, 2, &completed);
        check(completed == 2, "a CHECKSUM and its CANCEL both complete");
        sqe_checksum(&sq[0], 2, 0, R.slot_size, 0x42);
        check(run_one(R.fd, &sq[0]) >= 0, "  and the cancelled op released its slot");
    }
}

/* ------------------------------------------------------------------------- */

static void sigint_noop(int sig)
{
    (void)sig;
}

/* Block in ENTER, then die as told. The pipe is the readiness signal. */
static void blocked_child(int wfd, int catch_sigint, uint64_t ns)
{
    struct koru_ring m;
    struct koru_sqe s;
    struct koru_cqe c;
    struct koru_enter e;
    int r;

    if (catch_sigint)
        signal(SIGINT, sigint_noop);
    if (ring_open(&m, 32, 64, 4096, 8, 8) != 0)
        _exit(2);
    sqe_delay(&s, 0x5000, ns);
    enter_init(&e, &s, 1, &c, 1);
    e.min_complete = 1;
    if (write(wfd, "x", 1) != 1)
        _exit(2);
    r = ioctl(m.fd, KORU_IOC_ENTER, &e);
    if (!catch_sigint)
        _exit(0); /* SIGKILL case: getting here at all is a failure */
    if (r >= 0)
        _exit(3);
    if (errno != EINTR)
        _exit(4);
    if (e.submitted != 1)
        _exit(5); /* EINTR must still report what it consumed */
    _exit(0);
}

void sec_signals(void)
{
    int pipefd[2];
    pid_t pid;
    int status = 0;
    char b;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        failures++;
        return;
    }

    pid = fork();
    if (pid == 0)
        blocked_child(pipefd[1], 1, 30ull * 1000 * MS);
    if (read(pipefd[0], &b, 1) == 1)
        usleep(150000);
    kill(pid, SIGINT);
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "SIGINT during ENTER is EINTR, with submitted intact");
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        note("child exit code %d", WEXITSTATUS(status));

    pid = fork();
    if (pid == 0)
        blocked_child(pipefd[1], 0, 60ull * 1000 * MS);
    if (read(pipefd[0], &b, 1) == 1)
        usleep(150000);
    kill(pid, SIGKILL);
    status = 0;
    waitpid(pid, &status, 0);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "SIGKILL during ENTER kills the task, no D state");

    close(pipefd[0]);
    close(pipefd[1]);
}

/* OPEN resolves in the submitting task's context. In a kworker current_cred()
 * would be init_cred and this would succeed. */

static int creds_child(struct koru_ring *m)
{
    struct passwd *pw = getpwnam("nobody");
    uid_t nobody      = pw ? pw->pw_uid : 65534;
    gid_t nogroup     = pw ? pw->pw_gid : 65534;
    int64_t r;
    int bad = 0;

    /* VM_DONTCOPY: the parent leaves this ring unmapped, the child maps it. */
    if (ring_map(m) != 0)
        return 2;
    if (setgroups(0, NULL) != 0 || setresgid(nogroup, nogroup, nogroup) != 0 ||
        setresuid(nobody, nobody, nobody) != 0) {
        perror("drop privileges");
        return 2;
    }
    if (geteuid() == 0)
        return 2;

    r = r_open(m, 0, SHADOW, KORU_O_RDONLY);
    if (r != -EACCES) {
        fprintf(stderr, "  " SHADOW " as uid %u: res %lld, want %d\n", (unsigned)nobody,
                (long long)r, -EACCES);
        bad = 1;
    }
    r = r_open(m, 0, HOSTNAME, KORU_O_RDONLY);
    if (r <= 0) {
        fprintf(stderr, "  " HOSTNAME " as uid %u: res %lld, want a handle\n", (unsigned)nobody,
                (long long)r);
        bad = 1;
    }
    return bad;
}

void sec_creds(void)
{
    struct koru_ring m;
    struct stat sb;
    pid_t pid;
    int st = 0;

    if (geteuid() != 0) {
        printf("%-58s SKIP (not root)\n", "an unprivileged OPEN of " SHADOW " is EACCES");
        return;
    }
    if (stat(SHADOW, &sb) != 0 || (sb.st_mode & S_IROTH)) {
        printf("%-58s SKIP (" SHADOW " is world-readable)\n",
               "an unprivileged OPEN of " SHADOW " is EACCES");
        return;
    }
    if (ring_open(&m, 32, 64, 8192, 4, 8) != 0) {
        failures++;
        return;
    }

    pid = fork();
    if (pid == 0)
        _exit(creds_child(&m));
    if (pid < 0 || waitpid(pid, &st, 0) != pid) {
        perror("fork");
        failures++;
        ring_close(&m);
        return;
    }
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "an unprivileged submitter gets EACCES on " SHADOW);
    ring_close(&m);
}

/* T22: a one-shot poll, armed on the file's own waitqueue. */

#define POLLFIFO "/tmp/koru-check-pollfifo"

static void poll_udp(void);

/* An armed poll must produce nothing until its event arrives. */
static int poll_quiet(struct koru_ring *r, struct koru_cqe *cq, unsigned space)
{
    struct koru_enter e;

    memset(&e, 0, sizeof(e));
    e.cq_addr      = (uint64_t)(uintptr_t)cq;
    e.cq_space     = space;
    e.min_complete = 1;
    e.timeout_ns   = 100 * MS;
    if (ioctl(r->fd, KORU_IOC_ENTER, &e) < 0)
        return -1;
    return (int)e.completed;
}

/* Wait for one completion, up to a second. */
static int poll_wait_one(struct koru_ring *r, struct koru_cqe *cq, unsigned space)
{
    struct koru_enter e;

    memset(&e, 0, sizeof(e));
    e.cq_addr      = (uint64_t)(uintptr_t)cq;
    e.cq_space     = space;
    e.min_complete = 1;
    e.timeout_ns   = 1000 * MS;
    if (ioctl(r->fd, KORU_IOC_ENTER, &e) < 0)
        return -1;
    return (int)e.completed;
}

/* Arm on `handle`, then have `poke` make it ready. The FIFO and the socket
 * differ in exactly one thing: a socket wakes from softirq. */
static void poll_stream(struct koru_ring *r, int64_t h, int poke, const char *what)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    unsigned completed = 0;
    char label[96];

    sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_IN, 0x300);
    snprintf(label, sizeof(label), "%s: POLL_ADD arms without completing", what);
    check(submit(r->fd, sq, 1, cq, 4, 0, &completed) == 1 && completed == 0, label);

    snprintf(label, sizeof(label), "  and stays quiet while nothing is readable");
    check(poll_quiet(r, cq, 4) == 0, label);

    check(write(poke, "k", 1) == 1, "  the peer writes a byte");
    completed = (unsigned)poll_wait_one(r, cq, 4);
    if (completed == 1 && cq[0].user_data == 0x300) {
        check_res(cq[0].res, KORU_POLL_IN, "  and the poll completes with the read bit");
    } else {
        check(0, "  and the poll completes with the read bit");
        note("completed %u", completed);
    }
}

void sec_poll(void)
{
    struct koru_sqe sq[1];
    struct koru_cqe cq[8];
    const struct koru_cqe *t, *c;
    unsigned completed = 0;
    int sv[2], peer, rd;
    int64_t h;
    uint8_t buf[8];

    /* 1. A regular file has no poll method, so it is always ready. */
    h = r_open(&R, PATH_SLOT, PATFILE, KORU_O_RDONLY);
    check(h > 0, "OPEN a regular file");
    if (h > 0) {
        sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_IN, 0x200);
        check_res(run_one(R.fd, &sq[0]), KORU_POLL_IN,
                  "  POLL_ADD on it completes at once, readable");
        sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_OUT, 0x201);
        check_res(run_one(R.fd, &sq[0]), KORU_POLL_OUT, "  and writable, from the default mask");

        /* Rejection matrix. */
        sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_IN, 0x202);
        sq[0].off = 1;
        check_res(run_one(R.fd, &sq[0]), -EINVAL, "  POLL_ADD with a non-zero off is EINVAL");
        sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_IN, 0x203);
        sq[0].slot = 1;
        check_res(run_one(R.fd, &sq[0]), -EINVAL, "  a non-zero slot is EINVAL");
        sqe_poll(&sq[0], (uint32_t)h, 0, 0x204);
        check_res(run_one(R.fd, &sq[0]), -EINVAL, "  an empty event mask is EINVAL");
        sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_IN | (1u << 6), 0x205);
        check_res(run_one(R.fd, &sq[0]), -EINVAL, "  an unknown event bit is EINVAL");
        check_res(r_close(&R, (uint32_t)h), 0, "  and it closes");
    }
    sqe_poll(&sq[0], 0, KORU_POLL_IN, 0x206);
    check_res(run_one(R.fd, &sq[0]), -EBADF, "POLL_ADD on a zero handle is EBADF");

    /* 2. A FIFO: read-only, so pipe_poll asks for one waitqueue. */
    unlink(POLLFIFO);
    if (mkfifo(POLLFIFO, 0600) != 0) {
        check(0, "create the poll FIFO");
        return;
    }
    peer = open(POLLFIFO, O_RDWR | O_NONBLOCK);
    h    = peer < 0 ? -1 : r_open(&R, PATH_SLOT, POLLFIFO, KORU_O_RDONLY | KORU_O_NONBLOCK);
    check(h > 0, "OPEN the FIFO for reading");
    if (h > 0) {
        poll_stream(&R, h, peer, "FIFO");
        check(read(peer, buf, sizeof(buf)) == 1, "  and the byte is still there to read");

        /* 3. Cancel an armed poll. Nothing may complete afterwards. */
        sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_IN, 0x310);
        check(submit(R.fd, sq, 1, cq, 0, 0, &completed) == 1, "arm a second poll on the FIFO");
        sqe_cancel(&sq[0], 0x310, 0x311);
        check(submit(R.fd, sq, 1, cq, 8, 2, &completed) == 1 && completed == 2,
              "  the CANCEL and the poll both complete (C1)");
        t = find_cqe(cq, completed, 0x310);
        c = find_cqe(cq, completed, 0x311);
        if (t && c) {
            check_res(t->res, -ECANCELED, "  the poll gets -ECANCELED");
            check_res(c->res, 0, "  the canceller gets 0");
        } else {
            check(0, "  both CQEs carry their own user_data");
        }
        /* Without remove_wait_queue this is a use-after-free; without the
         * token it is a second completion, which breaks C1. */
        check(write(peer, "x", 1) == 1, "  the peer writes again");
        check(poll_quiet(&R, cq, 8) == 0, "  and the cancelled poll produces no CQE at all");
        check(read(peer, buf, sizeof(buf)) == 1, "  the byte is still readable");
        check_res(r_close(&R, (uint32_t)h), 0, "  and the handle closes");
    }

    /* 4. A pipe opened read-write wants two waitqueues, which is refused. */
    h = r_open(&R, PATH_SLOT, POLLFIFO, KORU_O_RDWR | KORU_O_NONBLOCK);
    check(h > 0, "OPEN the FIFO read-write");
    if (h > 0) {
        sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_IN, 0x320);
        check_res(run_one(R.fd, &sq[0]), -EOPNOTSUPP,
                  "  POLL_ADD on two waitqueues is EOPNOTSUPP");
        check_res(r_close(&R, (uint32_t)h), 0, "  and it closes");
    }
    if (peer >= 0)
        close(peer);
    unlink(POLLFIFO);

    /* 5. A stream socket. Its wake is the writer's own process context, like
     *    the FIFO's, but it reaches us through the socket layer. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        check(0, "create a socketpair");
        return;
    }
    h = r_adopt(&R, sv[0]);
    check(h > 0, "adopt one end of a socketpair");
    if (h > 0) {
        poll_stream(&R, h, sv[1], "socket");
        rd = (int)read(sv[0], buf, sizeof(buf));
        check(rd == 1, "  and the byte is still there to read");
        check_res(r_close(&R, (uint32_t)h), 0, "  and the handle closes");
    }
    close(sv[0]);
    close(sv[1]);

    /* 6. A UDP socket on loopback, which is the case the wake-callback rule is
     *    actually about: the datagram arrives in the NET_RX softirq, so the
     *    callback runs there rather than in any process's context. */
    poll_udp();
}

/* Bind a UDP socket on loopback and report where. */
static int udp_bind(struct sockaddr_in *addr)
{
    socklen_t len = sizeof(*addr);
    int fd        = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);

    if (fd < 0)
        return -1;
    memset(addr, 0, sizeof(*addr));
    addr->sin_family      = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)addr, sizeof(*addr)) != 0 ||
        getsockname(fd, (struct sockaddr *)addr, &len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void poll_udp(void)
{
    struct sockaddr_in addr;
    struct koru_sqe sq[1];
    struct koru_cqe cq[4];
    unsigned completed = 0;
    int rx, tx;
    int64_t h;
    uint8_t buf[8];

    rx = udp_bind(&addr);
    tx = rx < 0 ? -1 : socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0 || tx < 0) {
        check(0, "bind a UDP socket on loopback");
        if (rx >= 0)
            close(rx);
        return;
    }

    h = r_adopt(&R, rx);
    check(h > 0, "adopt a UDP socket");
    if (h > 0) {
        sqe_poll(&sq[0], (uint32_t)h, KORU_POLL_IN, 0x330);
        check(submit(R.fd, sq, 1, cq, 4, 0, &completed) == 1 && completed == 0,
              "  POLL_ADD on it arms without completing");
        check(sendto(tx, "k", 1, 0, (struct sockaddr *)&addr, sizeof(addr)) == 1,
              "  a datagram arrives through the softirq");
        completed = (unsigned)poll_wait_one(&R, cq, 4);
        if (completed == 1 && cq[0].user_data == 0x330)
            check_res(cq[0].res, KORU_POLL_IN, "  and the poll completes with the read bit");
        else
            check(0, "  and the poll completes with the read bit");
        check(recv(rx, buf, sizeof(buf), 0) == 1, "  and the datagram is still there");
        check_res(r_close(&R, (uint32_t)h), 0, "  and the handle closes");
    }
    close(rx);
    close(tx);
}
