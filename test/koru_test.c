// SPDX-License-Identifier: MIT
//
// Shared harness for koru_check, the integrated test.

#include "koru_test.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

int failures;

static void alarm_die(int sig)
{
    (void)sig;
    _exit(99);
}

void test_begin(unsigned alarm_secs)
{
    // _exit from the handler would drop a full buffer.
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (alarm_secs) {
        signal(SIGALRM, alarm_die);
        alarm(alarm_secs);
    }
}

int test_end(void)
{
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}

void check(int ok, const char *what)
{
    printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

void check_errno(int ret, int want, const char *what)
{
    if (ret >= 0) {
        printf("%-58s FAIL (succeeded, expected %s)\n", what, strerror(want));
        failures++;
    } else if (errno != want) {
        printf("%-58s FAIL (got %s, expected %s)\n", what, strerror(errno), strerror(want));
        failures++;
    } else {
        printf("%-58s PASS\n", what);
    }
}

void check_res(int64_t got, int64_t want, const char *what)
{
    if (got == want) {
        printf("%-58s PASS\n", what);
    } else {
        printf("%-58s FAIL (res %lld, want %lld)\n", what, (long long)got, (long long)want);
        failures++;
    }
}

void check_ge(long long got, long long want, const char *what)
{
    if (got >= want) {
        printf("%-58s PASS\n", what);
    } else {
        printf("%-58s FAIL (%lld, want >= %lld)\n", what, got, want);
        failures++;
    }
}

void note(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    printf("    ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

// ---------------------------------------------------------------------------

int open_dev(void)
{
    int fd = open(KORU_DEV, O_RDWR);

    if (fd < 0) {
        perror("open " KORU_DEV);
        failures++;
    }
    return fd;
}

int setup_ring(int fd, uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size,
               uint32_t slot_count)
{
    struct koru_params p;

    memset(&p, 0, sizeof(p));
    p.magic       = KORU_MAGIC;
    p.abi_version = KORU_ABI_VERSION;
    p.sq_entries  = sq_entries;
    p.cq_entries  = cq_entries;
    p.slot_size   = slot_size;
    p.slot_count  = slot_count;
    return ioctl(fd, KORU_IOC_SETUP, &p);
}

int ring_open(struct koru_ring *r, uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size,
              uint32_t slot_count, uint32_t handle_count)
{
    struct koru_params p;

    memset(r, 0, sizeof(*r));
    r->fd = open(KORU_DEV, O_RDWR);
    if (r->fd < 0) {
        perror("open " KORU_DEV);
        return -1;
    }
    memset(&p, 0, sizeof(p));
    p.magic        = KORU_MAGIC;
    p.abi_version  = KORU_ABI_VERSION;
    p.sq_entries   = sq_entries;
    p.cq_entries   = cq_entries;
    p.slot_size    = slot_size;
    p.slot_count   = slot_count;
    p.handle_count = handle_count;
    if (ioctl(r->fd, KORU_IOC_SETUP, &p) != 0) {
        perror("SETUP");
        close(r->fd);
        r->fd = -1;
        return -1;
    }
    r->sq_entries   = p.sq_entries;
    r->cq_entries   = p.cq_entries;
    r->slot_size    = p.slot_size;
    r->slot_count   = p.slot_count;
    r->handle_count = p.handle_count;
    r->arena_size   = p.arena_size;
    return 0;
}

int ring_map(struct koru_ring *r)
{
    void *a = mmap(NULL, r->arena_size, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);

    if (a == MAP_FAILED) {
        perror("mmap arena");
        return -1;
    }
    r->arena = a;
    return 0;
}

void ring_close(struct koru_ring *r)
{
    if (r->arena) {
        munmap(r->arena, r->arena_size);
        r->arena = NULL;
    }
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
}

int ring_quiesce(struct koru_ring *r)
{
    struct koru_cqe cq[128];
    struct koru_enter e;
    unsigned space = r->cq_entries < 128 ? r->cq_entries : 128;
    int total = 0, spin, idle = 0;

    // min_complete 1 returns at once on an idle ring, so the idle rounds are
    // free; on a busy one each costs the timeout, which is what lets a
    // still-running op land.
    for (spin = 0; spin < 64 && idle < 3; spin++) {
        memset(&e, 0, sizeof(e));
        e.cq_addr      = (uint64_t)(uintptr_t)cq;
        e.cq_space     = space;
        e.min_complete = 1;
        e.timeout_ns   = 50 * MS;
        if (ioctl(r->fd, KORU_IOC_ENTER, &e) < 0)
            break;
        if (e.completed == 0) {
            idle++;
            continue;
        }
        idle = 0;
        total += (int)e.completed;
    }
    return total;
}

// ---------------------------------------------------------------------------

void enter_init(struct koru_enter *e, struct koru_sqe *sq, unsigned n, struct koru_cqe *cq,
                unsigned cq_space)
{
    memset(e, 0, sizeof(*e));
    e->sq_addr   = (uint64_t)(uintptr_t)sq;
    e->to_submit = n;
    e->cq_addr   = (uint64_t)(uintptr_t)cq;
    e->cq_space  = cq_space;
}

int submit(int fd, struct koru_sqe *sq, unsigned n, struct koru_cqe *cq, unsigned cq_space,
           unsigned min_complete, unsigned *completed)
{
    struct koru_enter e;
    int r;

    enter_init(&e, sq, n, cq, cq_space);
    e.min_complete = min_complete;
    r              = ioctl(fd, KORU_IOC_ENTER, &e);
    if (completed)
        *completed = e.completed;
    return r;
}

int64_t run_one(int fd, struct koru_sqe *s)
{
    struct koru_cqe c;
    unsigned completed = 0;

    memset(&c, 0, sizeof(c));
    if (submit(fd, s, 1, &c, 1, 1, &completed) < 0 || completed != 1)
        return INT64_MIN;
    return c.res;
}

const struct koru_cqe *find_cqe(const struct koru_cqe *cq, unsigned n, uint64_t user_data)
{
    unsigned i;

    for (i = 0; i < n; i++)
        if (cq[i].user_data == user_data)
            return &cq[i];
    return NULL;
}

int64_t r_open(struct koru_ring *r, uint32_t slot, const char *path, uint32_t flags)
{
    struct koru_sqe s;
    uint32_t n = put_path(r->arena, r->slot_size, slot, path);

    sqe_open(&s, slot, 0, n, flags, 0x100);
    return run_one(r->fd, &s);
}

int64_t r_create(struct koru_ring *r, uint32_t slot, const char *path, uint32_t flags,
                 uint64_t mode)
{
    struct koru_sqe s;
    uint32_t n = put_path(r->arena, r->slot_size, slot, path);

    // The mode goes where every path op's argument goes.
    memcpy(r->arena + (size_t)slot * r->slot_size + arg_offset(0, n), &mode, sizeof(mode));
    sqe_open(&s, slot, 0, n, flags, 0x108);
    return run_one(r->fd, &s);
}

int64_t race_round(struct koru_ring *r, const struct koru_sqe *first,
                   const struct koru_sqe *second, uint64_t *extra)
{
    struct koru_sqe sq[2];
    struct koru_cqe cq[2];
    const struct koru_cqe *a, *b;
    unsigned completed = 0;

    sq[0] = *first;
    sq[1] = *second;
    if (submit(r->fd, sq, 2, cq, 2, 2, &completed) != 2 || completed != 2)
        return INT64_MIN;
    a = find_cqe(cq, completed, first->user_data);
    b = find_cqe(cq, completed, second->user_data);
    if (!a || !b)
        return INT64_MIN;
    if (extra)
        *extra = b->extra;
    return b->res;
}

int64_t r_close(struct koru_ring *r, uint32_t handle)
{
    struct koru_sqe s;

    sqe_close(&s, handle, 0x101);
    return run_one(r->fd, &s);
}

int64_t r_adopt(struct koru_ring *r, int fd)
{
    struct koru_sqe s;

    sqe_adopt(&s, fd, 0x104);
    return run_one(r->fd, &s);
}

int64_t r_read(struct koru_ring *r, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len)
{
    struct koru_sqe s;

    sqe_read(&s, handle, slot, off, len, 0x102);
    return run_one(r->fd, &s);
}

int64_t r_write(struct koru_ring *r, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len)
{
    struct koru_sqe s;

    sqe_write(&s, handle, slot, off, len, 0x103);
    return run_one(r->fd, &s);
}

int64_t r_path(struct koru_ring *r, uint8_t opcode, uint32_t slot, const char *path,
               const void *arg, size_t argsize)
{
    return r_path_extra(r, opcode, slot, path, arg, argsize, NULL);
}

int64_t r_path_extra(struct koru_ring *r, uint8_t opcode, uint32_t slot, const char *path,
                     const void *arg, size_t argsize, uint64_t *extra)
{
    struct koru_sqe s;
    struct koru_cqe c;
    unsigned completed = 0;
    uint32_t n         = put_path(r->arena, r->slot_size, slot, path);

    if (arg && argsize)
        memcpy(r->arena + (size_t)slot * r->slot_size + arg_offset(0, n), arg, argsize);
    sqe_path(&s, opcode, slot, 0, n, 0x106);
    if (submit(r->fd, &s, 1, &c, 1, 1, &completed) != 1 || completed != 1)
        return INT64_MIN;
    if (extra)
        *extra = c.extra;
    return c.res;
}

int64_t r_mkdir(struct koru_ring *r, uint32_t slot, const char *path, uint32_t mode)
{
    struct koru_sqe s;
    uint32_t n = put_path(r->arena, r->slot_size, slot, path);

    sqe_path(&s, KORU_OP_MKDIR, slot, 0, n, 0x107);
    s.handle = mode;
    return run_one(r->fd, &s);
}

int64_t r_symlink(struct koru_ring *r, uint32_t slot, const char *target, const char *link)
{
    struct koru_sqe s;
    uint32_t n = put_paths(r->arena, r->slot_size, slot, target, link);

    sqe_path(&s, KORU_OP_SYMLINK, slot, 0, n, 0x108);
    return run_one(r->fd, &s);
}

int64_t r_unlink(struct koru_ring *r, uint32_t slot, const char *path)
{
    return r_path(r, KORU_OP_UNLINK, slot, path, NULL, 0);
}

int64_t r_rmdir(struct koru_ring *r, uint32_t slot, const char *path)
{
    return r_path(r, KORU_OP_RMDIR, slot, path, NULL, 0);
}

int64_t r_rename(struct koru_ring *r, uint32_t slot, const char *from, const char *to)
{
    struct koru_sqe s;
    uint32_t n = put_paths(r->arena, r->slot_size, slot, from, to);

    sqe_path(&s, KORU_OP_RENAME, slot, 0, n, 0x109);
    return run_one(r->fd, &s);
}

int64_t r_stat(struct koru_ring *r, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len,
               uint64_t *extra)
{
    struct koru_sqe s;
    struct koru_cqe c;
    unsigned completed = 0;

    sqe_stat(&s, handle, slot, off, len, 0x105);
    if (submit(r->fd, &s, 1, &c, 1, 1, &completed) != 1 || completed != 1)
        return INT64_MIN;
    if (extra)
        *extra = c.extra;
    return c.res;
}

// ---------------------------------------------------------------------------

void sqe_nop(struct koru_sqe *s, uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_NOP;
    s->user_data = user_data;
}

void sqe_delay(struct koru_sqe *s, uint64_t user_data, uint64_t ns)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_DELAY_NS;
    s->off       = ns;
    s->user_data = user_data;
}

void sqe_checksum(struct koru_sqe *s, uint32_t slot, uint64_t off, uint32_t len, uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_CHECKSUM;
    s->slot      = slot;
    s->off       = off;
    s->len       = len;
    s->user_data = user_data;
}

void sqe_open(struct koru_sqe *s, uint32_t slot, uint64_t off, uint32_t len, uint32_t flags,
              uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_OPEN;
    s->slot      = slot;
    s->off       = off;
    s->len       = len;
    s->handle    = flags;
    s->user_data = user_data;
}

void sqe_close(struct koru_sqe *s, uint32_t handle, uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_CLOSE;
    s->handle    = handle;
    s->user_data = user_data;
}

void sqe_adopt(struct koru_sqe *s, int fd, uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_ADOPT_FD;
    s->off       = (uint64_t)fd;
    s->user_data = user_data;
}

void sqe_poll(struct koru_sqe *s, uint32_t handle, uint32_t events, uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_POLL_ADD;
    s->handle    = handle;
    s->len       = events;
    s->user_data = user_data;
}

void sqe_read(struct koru_sqe *s, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len,
              uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_READ;
    s->handle    = handle;
    s->slot      = slot;
    s->off       = off;
    s->len       = len;
    s->user_data = user_data;
}

void sqe_write(struct koru_sqe *s, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len,
               uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_WRITE;
    s->handle    = handle;
    s->slot      = slot;
    s->off       = off;
    s->len       = len;
    s->user_data = user_data;
}

void sqe_stat(struct koru_sqe *s, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len,
              uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_STAT;
    s->handle    = handle;
    s->slot      = slot;
    s->off       = off;
    s->len       = len;
    s->user_data = user_data;
}

void sqe_path(struct koru_sqe *s, uint8_t opcode, uint32_t slot, uint64_t off, uint32_t len,
              uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = opcode;
    s->slot      = slot;
    s->off       = off;
    s->len       = len;
    s->user_data = user_data;
}

uint64_t arg_offset(uint64_t off, uint32_t len)
{
    return (off + len + 7) & ~(uint64_t)7;
}

void sqe_cancel(struct koru_sqe *s, uint64_t target, uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_CANCEL;
    s->off       = target;
    s->user_data = user_data;
}

// ---------------------------------------------------------------------------

uint8_t pattern_byte(size_t i)
{
    return (uint8_t)(i * 31 + (i >> 8) * 7 + 11);
}

int make_pattern_file(const char *path, size_t n)
{
    uint8_t buf[4096];
    size_t done = 0;
    int fd      = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        perror(path);
        return -1;
    }
    while (done < n) {
        size_t i, chunk = n - done < sizeof(buf) ? n - done : sizeof(buf);

        for (i = 0; i < chunk; i++)
            buf[i] = pattern_byte(done + i);
        if (write(fd, buf, chunk) != (ssize_t)chunk) {
            perror("write");
            close(fd);
            return -1;
        }
        done += chunk;
    }
    close(fd);
    return 0;
}

uint32_t put_path(uint8_t *arena, uint32_t slot_size, uint32_t slot, const char *path)
{
    size_t n = strlen(path);

    memset(arena + (size_t)slot * slot_size, 0, slot_size);
    memcpy(arena + (size_t)slot * slot_size, path, n);
    return (uint32_t)n;
}

uint32_t put_paths(uint8_t *arena, uint32_t slot_size, uint32_t slot, const char *a, const char *b)
{
    uint8_t *p = arena + (size_t)slot * slot_size;
    size_t na = strlen(a), nb = strlen(b);

    memset(p, 0, slot_size);
    memcpy(p, a, na);
    memcpy(p + na + 1, b, nb);
    return (uint32_t)(na + 1 + nb);
}

long mem_free_kb(void)
{
    char line[256];
    FILE *f   = fopen("/proc/meminfo", "r");
    long kb   = -1;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemFree: %ld kB", &kb) == 1)
            break;
    fclose(f);
    return kb;
}

long file_nr(void)
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

long file_nr_settled(void)
{
    usleep(100000);
    return file_nr();
}

int count_fds(void)
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

uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint64_t wall_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return (int64_t)(h & 0x7fffffffffffffffull);
}
