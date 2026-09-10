// SPDX-License-Identifier: GPL-2.0
//
// T9 done test: OPEN and CLOSE, the generational handle table, and the creds
// regression.

#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "koru_test.h"

#define SLOT_SIZE  8192u /* > PATH_MAX, so the PATH_MAX clamp is reachable */
#define SLOT_COUNT 8u
#define ARENA      ((uint64_t)SLOT_SIZE * SLOT_COUNT)
#define HANDLES    8u
#define LEAK_ITERS 2000
#define PATH_MAX_  4096u

#define HOSTNAME "/etc/hostname"
#define SHADOW   "/etc/shadow"
#define LINKPATH "/tmp/koru-t9-symlink"

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

/* Field 1 of file-nr counts allocated struct file. kmemleak cannot see a leaked
 * handle: our own table still references it. */
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

/* fput can be deferred to task work, so let it settle before sampling. */
static long file_nr_settled(void)
{
    usleep(100000);
    return file_nr();
}

static int count_fds(void)
{
    struct dirent *e;
    DIR *d = opendir("/proc/self/fd");
    int n  = 0;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static int setup_p(int fd, struct koru_params *p, uint32_t handle_count)
{
    memset(p, 0, sizeof(*p));
    p->magic        = KORU_MAGIC;
    p->abi_version  = KORU_ABI_VERSION;
    p->sq_entries   = 32;
    p->cq_entries   = 64;
    p->slot_size    = SLOT_SIZE;
    p->slot_count   = SLOT_COUNT;
    p->handle_count = handle_count;
    return ioctl(fd, KORU_IOC_SETUP, p);
}

/* ------------------------------------------------------------------------- */

static void params_tests(void)
{
    struct koru_params p;
    uint32_t max_handles;
    int fd;

    fd = open_dev();
    if (fd < 0)
        return;
    memset(&p, 0, sizeof(p));
    p.magic       = KORU_MAGIC;
    p.abi_version = KORU_ABI_VERSION;
    check(ioctl(fd, KORU_IOC_GET_PARAMS, &p) == 0 && p.max_handles > 0 && p.handle_count == 0,
          "GET_PARAMS reports max_handles before SETUP");
    max_handles = p.max_handles;

    /* A rejected SETUP must not consume the one-shot. */
    check_errno(setup_p(fd, &p, max_handles + 1), EINVAL, "  handle_count past the cap is EINVAL");
    check(setup_p(fd, &p, 0) == 0 && p.handle_count > 0 && p.handle_count <= max_handles,
          "  handle_count 0 gets the kernel default");
    close(fd);

    fd = open_dev();
    if (fd < 0)
        return;
    check(setup_p(fd, &p, 7) == 0 && p.handle_count == 7, "  an explicit handle_count is echoed");
    close(fd);
}

static void handle_tests(int fd)
{
    struct koru_sqe s;
    int64_t h, h2;
    uint32_t idx, gen;

    h = do_open(fd, HOSTNAME, KORU_O_RDONLY);
    check(h > 0, "OPEN " HOSTNAME " returns a handle");
    if (h <= 0)
        return;
    idx = KORU_HANDLE_INDEX(h);
    gen = KORU_HANDLE_GEN(h);
    check(gen != 0 && idx < HANDLES, "  index inside the table, generation non-zero");
    check_res(do_close(fd, (uint32_t)h), 0, "  CLOSE retires it");

    check_res(do_close(fd, (uint32_t)h), -EBADF, "double CLOSE is EBADF");
    check_res(do_close(fd, 0), -EBADF, "CLOSE of handle 0 is EBADF");
    check_res(do_close(fd, idx | ((gen + 7) << 16)), -EBADF, "a stale generation is EBADF");
    check_res(do_close(fd, HANDLES | (1u << 16)), -EBADF, "an index past the table is EBADF");

    h2 = do_open(fd, HOSTNAME, KORU_O_RDONLY);
    check(h2 > 0 && KORU_HANDLE_INDEX(h2) == idx && KORU_HANDLE_GEN(h2) != gen,
          "the reused index carries a different generation");
    check_res(do_close(fd, (uint32_t)h), -EBADF, "  the old handle is still EBADF");
    if (h2 > 0)
        check_res(do_close(fd, (uint32_t)h2), 0, "  the new one closes");

    /* Fields CLOSE does not read must be zero. */
    h = do_open(fd, HOSTNAME, KORU_O_RDONLY);
    if (h > 0) {
        sqe_close(&s, (uint32_t)h, 0x200);
        s.len = 1;
        check_res(run_one(fd, &s), -EINVAL, "CLOSE with a non-zero len is EINVAL");
        sqe_close(&s, (uint32_t)h, 0x201);
        s.off = 1;
        check_res(run_one(fd, &s), -EINVAL, "  non-zero off is EINVAL");
        sqe_close(&s, (uint32_t)h, 0x202);
        s.slot = 1;
        check_res(run_one(fd, &s), -EINVAL, "  non-zero slot is EINVAL");
        check_res(do_close(fd, (uint32_t)h), 0, "  the handle survived all three");
    }
}

static void path_tests(int fd)
{
    struct koru_sqe s;
    uint32_t n, i;

    check_res(do_open(fd, "/no/such/path/here", KORU_O_RDONLY), -ENOENT,
              "a missing path is ENOENT");

    /* Embedded NUL, rejected in the kernel's own copy. */
    n        = put_path(arena, SLOT_SIZE, 0, HOSTNAME);
    arena[3] = 0;
    sqe_open(&s, 0, 0, n, KORU_O_RDONLY, 0x300);
    check_res(run_one(fd, &s), -EINVAL, "an embedded NUL is EINVAL");

    n = put_path(arena, SLOT_SIZE, 0, HOSTNAME);
    sqe_open(&s, 0, 0, 0, KORU_O_RDONLY, 0x301);
    check_res(run_one(fd, &s), -EINVAL, "a zero-length path is EINVAL");

    sqe_open(&s, 0, 0, SLOT_SIZE + 1, KORU_O_RDONLY, 0x302);
    check_res(run_one(fd, &s), -EINVAL, "len past the slot is EINVAL");

    sqe_open(&s, 0, SLOT_SIZE - 4, 8, KORU_O_RDONLY, 0x303);
    check_res(run_one(fd, &s), -EINVAL, "off + len past the slot is EINVAL");

    sqe_open(&s, SLOT_COUNT, 0, n, KORU_O_RDONLY, 0x304);
    check_res(run_one(fd, &s), -EINVAL, "a slot past the arena is EINVAL");

    /* "/a/a/a..." resolves nowhere, so the length clamp is what separates
     * PATH_MAX-1 from PATH_MAX. */
    for (i = 0; i < PATH_MAX_; i++)
        arena[i] = (i % 2) ? 'a' : '/';
    sqe_open(&s, 0, 0, PATH_MAX_ - 1, KORU_O_RDONLY, 0x305);
    check_res(run_one(fd, &s), -ENOENT, "a PATH_MAX-1 path is accepted and resolves");
    sqe_open(&s, 0, 0, PATH_MAX_, KORU_O_RDONLY, 0x306);
    check_res(run_one(fd, &s), -EINVAL, "  a PATH_MAX path is EINVAL");
}

static void flag_tests(int fd)
{
    int64_t h;

    check_res(do_open(fd, HOSTNAME, 1u << 8), -EINVAL, "an unknown open flag is EINVAL");
    check_res(do_open(fd, HOSTNAME, 3), -EINVAL, "access mode 3 is EINVAL");

    h = do_open(fd, "/etc", KORU_O_RDONLY | KORU_O_DIRECTORY);
    check(h > 0, "O_DIRECTORY on a directory succeeds");
    if (h > 0)
        check_res(do_close(fd, (uint32_t)h), 0, "  and closes");
    check_res(do_open(fd, HOSTNAME, KORU_O_RDONLY | KORU_O_DIRECTORY), -ENOTDIR,
              "  O_DIRECTORY on a regular file is ENOTDIR");
    check_res(do_open(fd, "/etc", KORU_O_WRONLY), -EISDIR, "O_WRONLY on a directory is EISDIR");

    unlink(LINKPATH);
    if (symlink(HOSTNAME, LINKPATH) == 0) {
        h = do_open(fd, LINKPATH, KORU_O_RDONLY);
        check(h > 0, "a symlink is followed by default");
        if (h > 0)
            do_close(fd, (uint32_t)h);
        check_res(do_open(fd, LINKPATH, KORU_O_RDONLY | KORU_O_NOFOLLOW), -ELOOP,
                  "  O_NOFOLLOW on a symlink is ELOOP");
        unlink(LINKPATH);
    } else {
        printf("%-58s SKIP (cannot create a symlink)\n", "O_NOFOLLOW");
    }
}

static void exhaustion_test(int fd)
{
    uint32_t held[HANDLES];
    int64_t h;
    unsigned i, ok = 0;

    for (i = 0; i < HANDLES; i++) {
        h = do_open(fd, HOSTNAME, KORU_O_RDONLY);
        if (h > 0) {
            held[i] = (uint32_t)h;
            ok++;
        } else {
            held[i] = 0;
        }
    }
    check(ok == HANDLES, "the whole handle table can be held at once");
    check_res(do_open(fd, HOSTNAME, KORU_O_RDONLY), -EMFILE, "  one more is EMFILE");

    if (held[3]) {
        uint32_t old = held[3];

        check_res(do_close(fd, old), 0, "  freeing one makes room again");
        h = do_open(fd, HOSTNAME, KORU_O_RDONLY);
        check(h > 0 && KORU_HANDLE_INDEX(h) == KORU_HANDLE_INDEX(old) &&
                  KORU_HANDLE_GEN(h) != KORU_HANDLE_GEN(old),
              "  the freed index returns with a new generation");
        check_res(do_close(fd, old), -EBADF, "  and the retired handle is EBADF");
        held[3] = h > 0 ? (uint32_t)h : 0;
    }

    for (i = 0; i < HANDLES; i++)
        if (held[i])
            do_close(fd, held[i]);
}

/* OPEN reads its path out of a slot, so it claims that slot like any other op. */
static void slot_test(int fd)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    unsigned completed = 0;
    uint32_t n;
    int ret;
    const struct koru_cqe *a = NULL, *b = NULL;
    unsigned i;

    n = put_path(arena, SLOT_SIZE, 1, HOSTNAME);
    sqe_checksum(&sq[0], 1, 0, SLOT_SIZE, 0xa0);
    sqe_open(&sq[1], 1, 0, n, KORU_O_RDONLY, 0xa1);
    ret = submit(fd, sq, 2, cq, 2, 2, &completed);
    for (i = 0; i < completed; i++) {
        if (cq[i].user_data == 0xa0)
            a = &cq[i];
        else if (cq[i].user_data == 0xa1)
            b = &cq[i];
    }
    check(ret == 2 && completed == 2 && a && b, "CHECKSUM and OPEN on one slot both complete");
    if (a && b) {
        check(a->res >= 0, "  the deferred CHECKSUM holds the slot");
        check_res(b->res, -EBUSY, "  the OPEN behind it gets EBUSY");
    }

    /* The slot is usable again once that completion posted. */
    {
        int64_t h = do_open(fd, HOSTNAME, KORU_O_RDONLY);

        check(h > 0, "  the slot frees when the CQE posts");
        if (h > 0)
            do_close(fd, (uint32_t)h);
    }
}

static void leak_test(int fd)
{
    long before, after;
    int fds_before, fds_after;
    int i, ok = 1;

    fds_before = count_fds();
    before     = file_nr_settled();
    for (i = 0; i < LEAK_ITERS; i++) {
        int64_t h = do_open(fd, HOSTNAME, KORU_O_RDONLY);

        if (h <= 0 || do_close(fd, (uint32_t)h) != 0) {
            ok = 0;
            break;
        }
    }
    check(ok, "2000 OPEN/CLOSE pairs all succeed");
    after     = file_nr_settled();
    fds_after = count_fds();

    check(fds_before >= 0 && fds_after == fds_before, "  /proc/self/fd is unchanged");
    check(before > 0 && after > 0 && after - before < 64,
          "  allocated struct file is unchanged: no handle leak");
    if (after - before >= 64)
        printf("    file-nr %ld -> %ld\n", before, after);
}

/* release() must drain the table even when a queued OpWork still holds the
 * ring's Arc. The 2 s delay is what keeps that Arc alive across the close. */
static void drain_test(void)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    unsigned completed = 0;
    long before, during, after;
    uint8_t *a;
    uint32_t n;
    int fd;

    before = file_nr_settled();
    fd     = open_ring_handles(32, 64, SLOT_SIZE, SLOT_COUNT, HANDLES);
    if (fd < 0)
        return;
    a = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (a == MAP_FAILED) {
        perror("mmap");
        failures++;
        close(fd);
        return;
    }

    n = put_path(a, SLOT_SIZE, 0, HOSTNAME);
    sqe_delay(&sq[0], 0xb0, 2000 * MS);
    sqe_open(&sq[1], 0, 0, n, KORU_O_RDONLY, 0xb1);
    check(submit(fd, sq, 2, cq, 2, 1, &completed) == 2 && completed == 1 && cq[0].res > 0,
          "a handle opened with a 2 s delay still queued");

    during = file_nr();
    check(during >= before + 2, "  the open handle and the ring fd are both counted");

    munmap(a, ARENA);
    close(fd);
    after = file_nr_settled();
    check(after <= before, "  closing the ring drops the handle at once, not in 2 s");
    if (after > before)
        printf("    file-nr %ld -> %ld -> %ld\n", before, during, after);
}

/* ------------------------------------------------------------------------- */

/* Runs in a forked child, after dropping to an unprivileged uid. OPEN resolves
 * in the submitting task's context, so this must fail on /etc/shadow. */
static int creds_child(int fd)
{
    struct passwd *pw = getpwnam("nobody");
    uid_t nobody      = pw ? pw->pw_uid : 65534;
    gid_t nogroup     = pw ? pw->pw_gid : 65534;
    struct koru_sqe s;
    int64_t r;
    uint32_t n;
    int bad = 0;

    arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (arena == MAP_FAILED) {
        perror("child mmap");
        return 2;
    }
    if (setgroups(0, NULL) != 0 || setresgid(nogroup, nogroup, nogroup) != 0 ||
        setresuid(nobody, nobody, nobody) != 0) {
        perror("drop privileges");
        return 2;
    }
    if (geteuid() == 0)
        return 2;

    n = put_path(arena, SLOT_SIZE, 0, SHADOW);
    sqe_open(&s, 0, 0, n, KORU_O_RDONLY, 0xc0);
    r = run_one(fd, &s);
    if (r != -EACCES) {
        fprintf(stderr, "  " SHADOW " as uid %u: res %lld, want %d\n", (unsigned)nobody,
                (long long)r, -EACCES);
        bad = 1;
    }

    n = put_path(arena, SLOT_SIZE, 0, HOSTNAME);
    sqe_open(&s, 0, 0, n, KORU_O_RDONLY, 0xc1);
    r = run_one(fd, &s);
    if (r <= 0) {
        fprintf(stderr, "  " HOSTNAME " as uid %u: res %lld, want a handle\n", (unsigned)nobody,
                (long long)r);
        bad = 1;
    }
    return bad;
}

static void creds_test(void)
{
    struct stat sb;
    pid_t pid;
    int st = 0, fd;

    if (geteuid() != 0) {
        printf("%-58s SKIP (not root)\n", "unprivileged OPEN of " SHADOW);
        return;
    }
    if (stat(SHADOW, &sb) != 0 || (sb.st_mode & S_IROTH)) {
        printf("%-58s SKIP (" SHADOW " is readable)\n", "unprivileged OPEN of " SHADOW);
        return;
    }

    /* The arena is VM_DONTCOPY, so the child cannot inherit the mapping. The
     * parent leaves this ring unmapped and the child maps it after the fork. */
    fd = open_ring_handles(32, 64, SLOT_SIZE, SLOT_COUNT, HANDLES);
    if (fd < 0)
        return;

    pid = fork();
    if (pid == 0)
        _exit(creds_child(fd));
    if (pid < 0) {
        perror("fork");
        failures++;
        close(fd);
        return;
    }
    if (waitpid(pid, &st, 0) != pid) {
        perror("waitpid");
        failures++;
        close(fd);
        return;
    }
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "an unprivileged submitter gets EACCES on " SHADOW);
    close(fd);
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    int fd;

    test_begin(300);

    params_tests();

    fd = open_ring_handles(32, 64, SLOT_SIZE, SLOT_COUNT, HANDLES);
    if (fd < 0)
        return 1;
    arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (arena == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    handle_tests(fd);
    path_tests(fd);
    flag_tests(fd);
    exhaustion_test(fd);
    slot_test(fd);
    leak_test(fd);

    munmap(arena, ARENA);
    close(fd);
    arena = NULL;

    drain_test();
    creds_test();

    return test_end();
}
