// SPDX-License-Identifier: MIT
//
// Per-opcode matrices, plus signals and credentials, which need a fork.
// Tail sections: they allocate almost nothing. See doc/Notes.md.

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
