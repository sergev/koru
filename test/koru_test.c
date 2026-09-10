// SPDX-License-Identifier: GPL-2.0
//
// Shared harness for the interim C tests.

#include "koru_test.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
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
    /* _exit from the handler would drop a full buffer, turning a hang into
     * a hang with no output saying where. */
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

int open_ring(uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size, uint32_t slot_count)
{
    return open_ring_handles(sq_entries, cq_entries, slot_size, slot_count, 0);
}

int open_ring_handles(uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size,
                      uint32_t slot_count, uint32_t handle_count)
{
    struct koru_params p;
    int fd = open_dev();

    if (fd < 0)
        return -1;
    memset(&p, 0, sizeof(p));
    p.magic        = KORU_MAGIC;
    p.abi_version  = KORU_ABI_VERSION;
    p.sq_entries   = sq_entries;
    p.cq_entries   = cq_entries;
    p.slot_size    = slot_size;
    p.slot_count   = slot_count;
    p.handle_count = handle_count;
    if (ioctl(fd, KORU_IOC_SETUP, &p) != 0) {
        perror("SETUP");
        failures++;
        close(fd);
        return -1;
    }
    return fd;
}

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

void sqe_cancel(struct koru_sqe *s, uint64_t target, uint64_t user_data)
{
    memset(s, 0, sizeof(*s));
    s->opcode    = KORU_OP_CANCEL;
    s->off       = target;
    s->user_data = user_data;
}

uint8_t pattern_byte(size_t i)
{
    return (uint8_t)(i * 31 + (i >> 8) * 7 + 11);
}

int make_pattern_file(const char *path, size_t n)
{
    uint8_t buf[1024];
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

uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
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
