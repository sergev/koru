// SPDX-License-Identifier: GPL-2.0
//
// T10 done test: READ into a slot, short reads at EOF, and a CLOSE racing an
// in-flight READ.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "koru_test.h"

#define SLOT_SIZE  8192u /* two pages, so the destination spans pages */
#define SLOT_COUNT 8u
#define ARENA      ((uint64_t)SLOT_SIZE * SLOT_COUNT)
#define HANDLES    8u
#define RACE_ITERS 3000

#define HOSTNAME "/etc/hostname"
#define PATFILE  "/tmp/koru-t10-pattern"
#define PATSIZE  20480u /* 5 pages: exercises the chunk loop */

static uint8_t *arena;

/* ------------------------------------------------------------------------- */

static int64_t do_open(int fd, const char *path, uint32_t flags)
{
    struct koru_sqe s;
    uint32_t n = put_path(arena, SLOT_SIZE, 0, path);

    sqe_open(&s, 0, 0, n, flags, 0x100);
    return run_one(fd, &s);
}

static int64_t do_close(int fd, uint32_t handle)
{
    struct koru_sqe s;

    sqe_close(&s, handle, 0x101);
    return run_one(fd, &s);
}

static int64_t do_read(int fd, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len)
{
    struct koru_sqe s;

    sqe_read(&s, handle, slot, off, len, 0x102);
    return run_one(fd, &s);
}

/* The slot's bytes must equal the pattern starting at file offset `off`. */
static int slot_matches(uint32_t slot, uint64_t off, size_t n)
{
    const uint8_t *p = arena + (size_t)slot * SLOT_SIZE;
    size_t i;

    for (i = 0; i < n; i++)
        if (p[i] != pattern_byte(off + i)) {
            printf("    mismatch at %zu: got %u, want %u\n", i, p[i], pattern_byte(off + i));
            return 0;
        }
    return 1;
}

static off_t file_size(const char *path)
{
    struct stat sb;

    return stat(path, &sb) == 0 ? sb.st_size : -1;
}

/* ------------------------------------------------------------------------- */

/* The whole point: the bytes must be what read(2) gives. */
static void hostname_test(int fd)
{
    uint8_t want[SLOT_SIZE];
    int64_t h, r;
    ssize_t n;
    int f;

    f = open(HOSTNAME, O_RDONLY);
    if (f < 0) {
        printf("%-58s SKIP (no " HOSTNAME ")\n", "READ matches read(2)");
        return;
    }
    n = read(f, want, sizeof(want));
    close(f);
    if (n <= 0) {
        printf("%-58s SKIP (empty)\n", "READ matches read(2)");
        return;
    }

    h = do_open(fd, HOSTNAME, KORU_O_RDONLY);
    if (h <= 0) {
        check(0, "READ matches read(2)");
        return;
    }
    memset(arena + SLOT_SIZE, 0, SLOT_SIZE);
    r = do_read(fd, (uint32_t)h, 1, 0, SLOT_SIZE);
    check(r == n && memcmp(arena + SLOT_SIZE, want, (size_t)n) == 0,
          "READ of " HOSTNAME " matches read(2) byte for byte");
    check_res(do_close(fd, (uint32_t)h), 0, "  and the handle closes");
}

static void pattern_tests(int fd)
{
    int64_t h, r;

    h = do_open(fd, PATFILE, KORU_O_RDONLY);
    check(h > 0, "OPEN " PATFILE);
    if (h <= 0)
        return;

    /* A read spanning several source chunks and two destination pages. */
    r = do_read(fd, (uint32_t)h, 1, 0, SLOT_SIZE);
    check(r == SLOT_SIZE && slot_matches(1, 0, SLOT_SIZE),
          "a two-page READ reproduces the pattern");

    /* Non-zero, non-page-aligned file offset. */
    r = do_read(fd, (uint32_t)h, 2, 4097, SLOT_SIZE);
    check(r == SLOT_SIZE && slot_matches(2, 4097, SLOT_SIZE),
          "  from an unaligned file offset too");

    /* Short read at EOF. */
    r = do_read(fd, (uint32_t)h, 3, PATSIZE - 100, SLOT_SIZE);
    check(r == 100 && slot_matches(3, PATSIZE - 100, 100),
          "a read past EOF is short, not an error");
    check_res(do_read(fd, (uint32_t)h, 3, PATSIZE, SLOT_SIZE), 0, "  at EOF exactly it is 0");
    check_res(do_read(fd, (uint32_t)h, 3, PATSIZE + 4096, SLOT_SIZE), 0, "  past EOF it is 0");

    /* A single byte, and the maximum the slot allows. */
    r = do_read(fd, (uint32_t)h, 4, 123, 1);
    check(r == 1 && slot_matches(4, 123, 1), "a one-byte READ works");

    check_res(do_close(fd, (uint32_t)h), 0, "  the handle closes");
}

static void reject_tests(int fd)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    unsigned completed = 0;
    int64_t h, stale;
    int ret;

    h = do_open(fd, PATFILE, KORU_O_RDONLY);
    if (h <= 0)
        return;

    check_res(do_read(fd, 0, 1, 0, 64), -EBADF, "READ with handle 0 is EBADF");
    check_res(do_read(fd, (uint32_t)h + (1u << 16), 1, 0, 64), -EBADF,
              "  a stale generation is EBADF");
    check_res(do_read(fd, HANDLES | (1u << 16), 1, 0, 64), -EBADF,
              "  an index past the table is EBADF");

    check_res(do_read(fd, (uint32_t)h, 1, 0, 0), -EINVAL, "a zero-length READ is EINVAL");
    check_res(do_read(fd, (uint32_t)h, 1, 0, SLOT_SIZE + 1), -EINVAL,
              "  len past the slot is EINVAL");
    check_res(do_read(fd, (uint32_t)h, SLOT_COUNT, 0, 64), -EINVAL,
              "  a slot past the arena is EINVAL");
    check_res(do_read(fd, (uint32_t)h, 1, (uint64_t)1 << 63, 64), -EINVAL,
              "  a negative file offset is EINVAL");

    /* A CHECKSUM holding the slot refuses the READ behind it. */
    sqe_checksum(&sq[0], 5, 0, SLOT_SIZE, 0xa0);
    sqe_read(&sq[1], (uint32_t)h, 5, 0, 64, 0xa1);
    ret = submit(fd, sq, 2, cq, 2, 2, &completed);
    check(ret == 2 && completed == 2, "CHECKSUM and READ on one slot both complete");
    if (completed == 2) {
        const struct koru_cqe *b = cq[0].user_data == 0xa1 ? &cq[0] : &cq[1];

        check_res(b->res, -EBUSY, "  the READ behind it gets EBUSY");
    }

    stale = h;
    check_res(do_close(fd, (uint32_t)h), 0, "the handle closes");
    check_res(do_read(fd, (uint32_t)stale, 1, 0, 64), -EBADF, "  a closed handle is EBADF");
}

/* A directory is rejected before kernel_read can WARN about it. */
static void directory_test(int fd)
{
    int64_t h;

    h = do_open(fd, "/etc", KORU_O_RDONLY | KORU_O_DIRECTORY);
    check(h > 0, "OPEN /etc as a directory");
    if (h <= 0)
        return;
    check_res(do_read(fd, (uint32_t)h, 1, 0, 64), -EINVAL, "  READ of a directory is EINVAL");
    check_res(do_close(fd, (uint32_t)h), 0, "  and it closes");
}

/* OPEN now takes regular files and directories only. */
static void filetype_test(int fd)
{
    check_res(do_open(fd, "/dev/null", KORU_O_RDONLY), -EINVAL, "OPEN of a device node is EINVAL");
}

/* The whole reason the ARef is resolved at submit time: CLOSE runs inline while
 * the READ is still queued, so the work item must own its own reference. */
static void race_test(int fd)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    unsigned completed;
    int i, bad = 0;

    for (i = 0; i < RACE_ITERS; i++) {
        int64_t h = do_open(fd, PATFILE, KORU_O_RDONLY);

        if (h <= 0) {
            bad = 1;
            break;
        }
        memset(arena + SLOT_SIZE, 0, SLOT_SIZE);
        sqe_read(&sq[0], (uint32_t)h, 1, 0, SLOT_SIZE, 0xb0);
        sqe_close(&sq[1], (uint32_t)h, 0xb1);
        if (submit(fd, sq, 2, cq, 2, 2, &completed) != 2 || completed != 2) {
            bad = 1;
            break;
        }
        for (unsigned j = 0; j < completed; j++) {
            if (cq[j].user_data == 0xb0 && cq[j].res != SLOT_SIZE)
                bad = 1;
            if (cq[j].user_data == 0xb1 && cq[j].res != 0)
                bad = 1;
        }
        if (bad)
            break;
    }
    check(!bad, "3000 CLOSEs racing an in-flight READ all complete");
    check(slot_matches(1, 0, SLOT_SIZE), "  and the last one still read the right bytes");
}

/* The T9 drain, now against a work item holding a file as well as the ring. */
static void teardown_race_test(void)
{
    struct koru_sqe sq[3];
    struct koru_cqe cq[3];
    unsigned completed = 0;
    uint8_t *a;
    uint32_t n;
    int fd, i;

    for (i = 0; i < 200; i++) {
        fd = open_ring_handles(32, 64, SLOT_SIZE, SLOT_COUNT, HANDLES);
        if (fd < 0)
            return;
        a = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (a == MAP_FAILED) {
            perror("mmap");
            failures++;
            close(fd);
            return;
        }
        n = put_path(a, SLOT_SIZE, 0, PATFILE);
        sqe_open(&sq[0], 0, 0, n, KORU_O_RDONLY, 0xc0);
        if (submit(fd, sq, 1, cq, 1, 1, &completed) != 1 || completed != 1 || cq[0].res <= 0) {
            failures++;
            munmap(a, ARENA);
            close(fd);
            return;
        }
        /* Queue the READ and walk away without reaping it. */
        sqe_read(&sq[0], (uint32_t)cq[0].res, 1, 0, SLOT_SIZE, 0xc1);
        submit(fd, sq, 1, cq, 1, 0, &completed);
        munmap(a, ARENA);
        close(fd);
    }
    check(1, "200 ring teardowns with a READ still in flight");
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    int fd;

    test_begin(300);

    if (make_pattern_file(PATFILE, PATSIZE) != 0)
        return 1;
    check(file_size(PATFILE) == PATSIZE, "the pattern file is the size we wrote");

    fd = open_ring_handles(32, 64, SLOT_SIZE, SLOT_COUNT, HANDLES);
    if (fd < 0)
        return 1;
    arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (arena == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    hostname_test(fd);
    pattern_tests(fd);
    reject_tests(fd);
    directory_test(fd);
    filetype_test(fd);
    race_test(fd);

    munmap(arena, ARENA);
    close(fd);
    arena = NULL;

    teardown_race_test();

    unlink(PATFILE);
    return test_end();
}
