/* SPDX-License-Identifier: MIT */
/* Shared harness for koru_check, the integrated test. */

#ifndef KORU_TEST_H
#define KORU_TEST_H

#include <stdint.h>

/* Angle brackets on purpose: with quotes this directory is searched first, so
 * a stale local copy would silently shadow the real mirror. */
#include <koru_abi.h>

#define MS 1000000ull

extern int failures;

/* Line-buffer stdout and arm a watchdog; 0 secs means no alarm. */
void test_begin(unsigned alarm_secs);
int test_end(void);

void check(int ok, const char *what);
/* Assert the exact errno, never just that the call failed. */
void check_errno(int ret, int want, const char *what);
/* Assert an exact cqe.res, reporting both values on a mismatch. */
void check_res(int64_t got, int64_t want, const char *what);
void check_ge(long long got, long long want, const char *what);
/* An indented informational line. Never a verdict. */
void note(const char *fmt, ...);

/* ---------------------------------------------------------------------------
 * Rings.
 * ------------------------------------------------------------------------ */

struct koru_ring {
    int fd;
    uint8_t *arena; /* NULL until ring_map */
    uint32_t sq_entries, cq_entries, slot_size, slot_count, handle_count;
    uint64_t arena_size;
};

/* SETUP a ring on a fresh fd. handle_count 0 takes the kernel default. */
int ring_open(struct koru_ring *r, uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size,
              uint32_t slot_count, uint32_t handle_count);
int ring_map(struct koru_ring *r);
void ring_close(struct koru_ring *r);
/* Reap everything and wait out anything still running. Returns the count found. */
int ring_quiesce(struct koru_ring *r);

int open_dev(void);
int setup_ring(int fd, uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size,
               uint32_t slot_count);

void enter_init(struct koru_enter *e, struct koru_sqe *sq, unsigned n, struct koru_cqe *cq,
                unsigned cq_space);
int submit(int fd, struct koru_sqe *sq, unsigned n, struct koru_cqe *cq, unsigned cq_space,
           unsigned min_complete, unsigned *completed);
/* One SQE to completion. Returns its res, or INT64_MIN if it never completed. */
int64_t run_one(int fd, struct koru_sqe *s);
/* Find a completion by user_data, or NULL. */
const struct koru_cqe *find_cqe(const struct koru_cqe *cq, unsigned n, uint64_t user_data);

/* Whole ops on a mapped ring, each to completion. `slot` is the caller's. */
int64_t r_open(struct koru_ring *r, uint32_t slot, const char *path, uint32_t flags);
int64_t r_close(struct koru_ring *r, uint32_t handle);
int64_t r_adopt(struct koru_ring *r, int fd);
int64_t r_read(struct koru_ring *r, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len);
int64_t r_write(struct koru_ring *r, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len);
/* MKDIR carries its mode in handle, as OPEN carries its flags. */
int64_t r_mkdir(struct koru_ring *r, uint32_t slot, const char *path, uint32_t mode);
/* SYMLINK takes both paths in the slot, target first. */
int64_t r_symlink(struct koru_ring *r, uint32_t slot, const char *target, const char *link);
/* RENAME takes both paths in the slot, the old one first. */
int64_t r_rename(struct koru_ring *r, uint32_t slot, const char *from, const char *to);
int64_t r_unlink(struct koru_ring *r, uint32_t slot, const char *path);
int64_t r_rmdir(struct koru_ring *r, uint32_t slot, const char *path);
/* `extra` may be NULL; on success it gets the CQE's KORU_STAT_* mask. */
int64_t r_stat(struct koru_ring *r, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len,
               uint64_t *extra);

void sqe_nop(struct koru_sqe *s, uint64_t user_data);
void sqe_delay(struct koru_sqe *s, uint64_t user_data, uint64_t ns);
void sqe_checksum(struct koru_sqe *s, uint32_t slot, uint64_t off, uint32_t len,
                  uint64_t user_data);
void sqe_open(struct koru_sqe *s, uint32_t slot, uint64_t off, uint32_t len, uint32_t flags,
              uint64_t user_data);
void sqe_close(struct koru_sqe *s, uint32_t handle, uint64_t user_data);
void sqe_adopt(struct koru_sqe *s, int fd, uint64_t user_data);
void sqe_poll(struct koru_sqe *s, uint32_t handle, uint32_t events, uint64_t user_data);
void sqe_read(struct koru_sqe *s, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len,
              uint64_t user_data);
void sqe_write(struct koru_sqe *s, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len,
               uint64_t user_data);
void sqe_stat(struct koru_sqe *s, uint32_t handle, uint32_t slot, uint64_t off, uint32_t len,
              uint64_t user_data);
/* A path op: path at (off, len), argument after it. `opcode` is one of
 * KORU_OP_TRUNCATE, KORU_OP_UTIMES, KORU_OP_READLINK, KORU_OP_STATX_AT,
 * KORU_OP_MKDIR, KORU_OP_SYMLINK, KORU_OP_UNLINK, KORU_OP_RMDIR,
 * KORU_OP_RENAME. */
void sqe_path(struct koru_sqe *s, uint8_t opcode, uint32_t slot, uint64_t off, uint32_t len,
              uint64_t user_data);
/* Where a path op's argument goes: the first 8-aligned offset at or after the
 * end of the path. The kernel computes the same thing. */
uint64_t arg_offset(uint64_t off, uint32_t len);
/* `target` is the user_data of the op to cancel. */
void sqe_cancel(struct koru_sqe *s, uint64_t target, uint64_t user_data);

/* Write `n` bytes of a position-dependent pattern to `path`. Returns 0 on
 * success. The pattern is what a READ of that file must reproduce. */
int make_pattern_file(const char *path, size_t n);
uint8_t pattern_byte(size_t i);

/* Copy a path into a slot, without its NUL. Returns its length. */
uint32_t put_path(uint8_t *arena, uint32_t slot_size, uint32_t slot, const char *path);
/* Two paths back to back with one NUL between them, as SYMLINK wants. Returns
 * the byte count covering both and the separator. */
uint32_t put_paths(uint8_t *arena, uint32_t slot_size, uint32_t slot, const char *a,
                   const char *b);

/* Run one path op on a mapped ring: put `path` at slot offset 0, copy `arg`
 * after it, submit, return its res. `arg` may be NULL. */
int64_t r_path(struct koru_ring *r, uint8_t opcode, uint32_t slot, const char *path,
               const void *arg, size_t argsize);
/* The same, keeping the CQE's extra. `extra` may be NULL. */
int64_t r_path_extra(struct koru_ring *r, uint8_t opcode, uint32_t slot, const char *path,
                     const void *arg, size_t argsize, uint64_t *extra);

/* MemFree from /proc/meminfo, in KB, or -1. */
long mem_free_kb(void);
/* Field 1 of /proc/sys/fs/file-nr: allocated struct file. */
long file_nr(void);
/* fput can be deferred to task work, so let it settle before sampling. */
long file_nr_settled(void);
int count_fds(void);

uint64_t now_ms(void);  /* CLOCK_MONOTONIC, for intervals */
uint64_t wall_ms(void); /* CLOCK_REALTIME, for the leak window */
/* Must match the kernel's FNV-1a, masked to 63 bits. */
int64_t fnv1a(const uint8_t *p, size_t n);

#endif /* KORU_TEST_H */
