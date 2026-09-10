/* SPDX-License-Identifier: GPL-2.0 */
/* Shared harness for the interim C tests. */

#ifndef XRING_TEST_H
#define XRING_TEST_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

/* ---------------------------------------------------------------------------
 * ABI mirror. Hand-written copy of kernel/xring_abi.rs, which is canonical.
 * T16 deletes this section and includes user/cpp/include/xring_abi.h instead.
 * Until then, a change there means a change here; the asserts below are what
 * catch you forgetting.
 * ------------------------------------------------------------------------ */

#define XRING_DEV "/dev/xring"
#define XRING_MAGIC 0x676e7278u /* "xrng" */
#define XRING_ABI_VERSION 1u

struct xring_params {
	/* in */
	uint32_t magic, abi_version, flags;
	/* in/out */
	uint32_t sq_entries, cq_entries, slot_size, slot_count;
	/* out */
	uint32_t configured;
	uint64_t features, arena_size;
	uint32_t max_sq_entries, max_cq_entries, max_slot_size, max_slot_count;
	uint64_t max_arena_bytes;
	/* in: must be zero */
	uint64_t reserved[4];
};

struct xring_sqe {
	uint8_t opcode, flags;
	uint16_t rsvd0;
	uint32_t len;
	uint64_t off;
	uint64_t user_data;
	uint32_t slot, handle;
};

struct xring_cqe {
	uint64_t user_data;
	int64_t res;
	uint32_t flags, rsvd0;
	uint64_t extra;
};

struct xring_enter {
	uint64_t sq_addr, cq_addr, timeout_ns;
	uint32_t to_submit, cq_space, min_complete, flags;
	/* out */
	uint32_t completed, submitted;
	/* in: must be zero */
	uint64_t reserved[2];
};

_Static_assert(sizeof(struct xring_params) == 104, "params size");
_Static_assert(sizeof(struct xring_sqe) == 32, "sqe size");
_Static_assert(sizeof(struct xring_cqe) == 32, "cqe size");
_Static_assert(sizeof(struct xring_enter) == 64, "enter size");
_Static_assert(offsetof(struct xring_sqe, off) == 8, "sqe.off");
_Static_assert(offsetof(struct xring_sqe, user_data) == 16, "sqe.user_data");
_Static_assert(offsetof(struct xring_sqe, slot) == 24, "sqe.slot");
_Static_assert(offsetof(struct xring_cqe, res) == 8, "cqe.res");
_Static_assert(offsetof(struct xring_cqe, extra) == 24, "cqe.extra");
_Static_assert(offsetof(struct xring_enter, to_submit) == 24, "enter.to_submit");
_Static_assert(offsetof(struct xring_enter, completed) == 40, "enter.completed");
_Static_assert(offsetof(struct xring_enter, submitted) == 44, "enter.submitted");

#define XRING_IOC_SETUP _IOWR('x', 0x00, struct xring_params)
#define XRING_IOC_GET_PARAMS _IOR('x', 0x01, struct xring_params)
#define XRING_IOC_ENTER _IOWR('x', 0x02, struct xring_enter)

#define XRING_OP_NOP 0
#define XRING_OP_DELAY_NS 1
#define XRING_OP_OPEN 2
#define XRING_OP_READ 3
#define XRING_OP_CLOSE 4
#define XRING_OP_CANCEL 5
#define XRING_OP_CHECKSUM 6

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

int open_dev(void);
int setup_ring(int fd, uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size,
	       uint32_t slot_count);
int open_ring(uint32_t sq_entries, uint32_t cq_entries, uint32_t slot_size,
	      uint32_t slot_count);

void enter_init(struct xring_enter *e, struct xring_sqe *sq, unsigned n, struct xring_cqe *cq,
		unsigned cq_space);
int submit(int fd, struct xring_sqe *sq, unsigned n, struct xring_cqe *cq, unsigned cq_space,
	   unsigned min_complete, unsigned *completed);
/* One SQE to completion. Returns its res, or INT64_MIN if it never completed. */
int64_t run_one(int fd, struct xring_sqe *s);

void sqe_nop(struct xring_sqe *s, uint64_t user_data);
void sqe_delay(struct xring_sqe *s, uint64_t user_data, uint64_t ns);
void sqe_checksum(struct xring_sqe *s, uint32_t slot, uint64_t off, uint32_t len,
		  uint64_t user_data);

uint64_t now_ms(void);
/* Must match the kernel's FNV-1a, masked to 63 bits. */
int64_t fnv1a(const uint8_t *p, size_t n);

#endif /* XRING_TEST_H */
