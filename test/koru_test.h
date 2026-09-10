/* SPDX-License-Identifier: GPL-2.0 */
/* Shared harness for the interim C tests. */

#ifndef KORU_TEST_H
#define KORU_TEST_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

/* ---------------------------------------------------------------------------
 * ABI mirror. Hand-written copy of kernel/koru_abi.rs, which is canonical.
 * T16 deletes this section and includes user/cpp/include/koru_abi.h instead.
 * Until then, a change there means a change here; the asserts below are what
 * catch you forgetting.
 * ------------------------------------------------------------------------ */

#define KORU_DEV         "/dev/koru"
#define KORU_MAGIC       0x75726f6bu /* "koru" */
#define KORU_ABI_VERSION 1u

struct koru_params {
    /* in */
    uint32_t magic, abi_version, flags;
    /* in/out */
    uint32_t sq_entries, cq_entries, slot_size, slot_count;
    /* out */
    uint32_t configured;
    uint64_t features, arena_size;
    uint32_t max_sq_entries, max_cq_entries, max_slot_size, max_slot_count;
    uint64_t max_arena_bytes;
    /* in/out: 0 means the kernel default */
    uint32_t handle_count;
    /* out */
    uint32_t max_handles;
    /* in: must be zero */
    uint64_t reserved[3];
};

struct koru_sqe {
    uint8_t opcode, flags;
    uint16_t rsvd0;
    uint32_t len;
    uint64_t off;
    uint64_t user_data;
    uint32_t slot, handle;
};

struct koru_cqe {
    uint64_t user_data;
    int64_t res;
    uint32_t flags, rsvd0;
    uint64_t extra;
};

struct koru_enter {
    uint64_t sq_addr, cq_addr, timeout_ns;
    uint32_t to_submit, cq_space, min_complete, flags;
    /* out */
    uint32_t completed, submitted;
    /* in: must be zero */
    uint64_t reserved[2];
};

_Static_assert(sizeof(struct koru_params) == 104, "params size");
_Static_assert(sizeof(struct koru_sqe) == 32, "sqe size");
_Static_assert(sizeof(struct koru_cqe) == 32, "cqe size");
_Static_assert(sizeof(struct koru_enter) == 64, "enter size");
_Static_assert(offsetof(struct koru_sqe, off) == 8, "sqe.off");
_Static_assert(offsetof(struct koru_sqe, user_data) == 16, "sqe.user_data");
_Static_assert(offsetof(struct koru_sqe, slot) == 24, "sqe.slot");
_Static_assert(offsetof(struct koru_cqe, res) == 8, "cqe.res");
_Static_assert(offsetof(struct koru_cqe, extra) == 24, "cqe.extra");
_Static_assert(offsetof(struct koru_enter, to_submit) == 24, "enter.to_submit");
_Static_assert(offsetof(struct koru_enter, completed) == 40, "enter.completed");
_Static_assert(offsetof(struct koru_enter, submitted) == 44, "enter.submitted");
_Static_assert(offsetof(struct koru_params, handle_count) == 72, "params.handle_count");
_Static_assert(offsetof(struct koru_params, max_handles) == 76, "params.max_handles");
_Static_assert(offsetof(struct koru_params, reserved) == 80, "params.reserved");

#define KORU_IOC_SETUP      _IOWR('k', 0x00, struct koru_params)
#define KORU_IOC_GET_PARAMS _IOR('k', 0x01, struct koru_params)
#define KORU_IOC_ENTER      _IOWR('k', 0x02, struct koru_enter)

#define KORU_OP_NOP      0
#define KORU_OP_DELAY_NS 1
#define KORU_OP_OPEN     2
#define KORU_OP_READ     3
#define KORU_OP_CLOSE    4
#define KORU_OP_CANCEL   5
#define KORU_OP_CHECKSUM 6

/* Open flags, carried in an OPEN SQE's handle field. */
#define KORU_O_ACCMODE   0x3u
#define KORU_O_RDONLY    0u
#define KORU_O_WRONLY    1u
#define KORU_O_RDWR      2u
#define KORU_O_NOFOLLOW  (1u << 2)
#define KORU_O_DIRECTORY (1u << 3)

/* Handle encoding: index in the low half, generation in the high half. */
#define KORU_HANDLE_INDEX(h) ((uint32_t)(h) & 0xffffu)
#define KORU_HANDLE_GEN(h)   ((uint32_t)(h) >> 16)

/* ---------------------------------------------------------------------------
 * Harness.
 * ------------------------------------------------------------------------ */

#define MS 1000000ull

extern int failures;

/* Line-buffer stdout and arm a watchdog; 0 secs means no alarm. A hang must
 * fail with its output intact, not wedge the run. */
void test_begin(unsigned alarm_secs);
int test_end(void);

void check(int ok, const char *what);
/* Assert the exact errno, never just that the call failed. */
void check_errno(int ret, int want, const char *what);
/* Assert an exact cqe.res, reporting both values on a mismatch. */
void check_res(int64_t got, int64_t want, const char *what);

int open_dev(void);
int setup_ring(int fd, uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size,
               uint32_t slot_count);
int open_ring(uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size, uint32_t slot_count);
/* As open_ring, but sizes the handle table too; 0 means the kernel default. */
int open_ring_handles(uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size,
                      uint32_t slot_count, uint32_t handle_count);

void enter_init(struct koru_enter *e, struct koru_sqe *sq, unsigned n, struct koru_cqe *cq,
                unsigned cq_space);
int submit(int fd, struct koru_sqe *sq, unsigned n, struct koru_cqe *cq, unsigned cq_space,
           unsigned min_complete, unsigned *completed);
/* One SQE to completion. Returns its res, or INT64_MIN if it never completed. */
int64_t run_one(int fd, struct koru_sqe *s);

void sqe_nop(struct koru_sqe *s, uint64_t user_data);
void sqe_delay(struct koru_sqe *s, uint64_t user_data, uint64_t ns);
void sqe_checksum(struct koru_sqe *s, uint32_t slot, uint64_t off, uint32_t len,
                  uint64_t user_data);
void sqe_open(struct koru_sqe *s, uint32_t slot, uint64_t off, uint32_t len, uint32_t flags,
              uint64_t user_data);
void sqe_close(struct koru_sqe *s, uint32_t handle, uint64_t user_data);

/* Copy a path into a slot, without its NUL. Returns its length. */
uint32_t put_path(uint8_t *arena, uint32_t slot_size, uint32_t slot, const char *path);

uint64_t now_ms(void);
/* Must match the kernel's FNV-1a, masked to 63 bits. */
int64_t fnv1a(const uint8_t *p, size_t n);

#endif /* KORU_TEST_H */
