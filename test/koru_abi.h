/* SPDX-License-Identifier: GPL-2.0 */
/* Hand-written mirror of kernel/koru_abi.rs, which is canonical. A change there
 * means a change here; the asserts below are what catch you forgetting.
 * T14 replaces this file with cpp/include/koru_abi.h. */

#ifndef KORU_ABI_H
#define KORU_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

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
    uint64_t max_delay_ns;
    /* in: must be zero */
    uint64_t reserved[2];
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
_Static_assert(offsetof(struct koru_params, max_delay_ns) == 80, "params.max_delay_ns");
_Static_assert(offsetof(struct koru_params, reserved) == 88, "params.reserved");

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

#endif /* KORU_ABI_H */
