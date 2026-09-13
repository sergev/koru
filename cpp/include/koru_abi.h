/* SPDX-License-Identifier: MIT */
/*
 * The koru wire format. Hand-written mirror of kernel/koru_abi.rs, which is
 * canonical. A change there means a change here, and scripts/abi.sh diffs this
 * file against rust/sys/src/abi.rs through the two abi_dump binaries.
 *
 * Two rules hold everywhere: reserved fields must be zero and unknown flag bits
 * are rejected, and nothing has padding.
 *
 * Valid C11 and C++20 both. Every constant is a #define, never a static const:
 * an unused static const is -Wunused-const-variable in C, which is -Werror in
 * test/.
 */

#ifndef KORU_ABI_H
#define KORU_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
#define KORU_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#define KORU_ALIGNOF(type)            alignof(type)
#else
#define KORU_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define KORU_ALIGNOF(type)            _Alignof(type)
#endif

/* The device node. Mirrors koru_sys::ring::DEV_KORU. */
#define KORU_DEV "/dev/koru"

/* Set by userspace in koru_params.magic. Spells "koru" little-endian. */
#define KORU_MAGIC 0x75726f6bu

/* Bumped on any incompatible change to this file. */
#define KORU_ABI_VERSION 1u

/* ioctl type byte. */
#define KORU_IOC_TYPE 'k'

/* Command numbers. Dispatch is on these, not the whole ioctl number, which
 * keeps ENOTTY (no such command) distinct from EPROTO (wrong struct size). */
#define KORU_NR_SETUP      0x00u
#define KORU_NR_GET_PARAMS 0x01u
#define KORU_NR_ENTER      0x02u

/* No flags are defined yet on either, so any bit set is rejected. */
#define KORU_SETUP_FLAGS_ALL 0u
#define KORU_ENTER_FLAGS_ALL 0u

/* Caps, reported in every koru_params. A request above one is rejected, not
 * clamped. The arena is unaccounted memory, so its cap is load-bearing. */
#define KORU_MAX_SQ_ENTRIES  4096u
#define KORU_MAX_CQ_ENTRIES  8192u
#define KORU_MAX_SLOT_SIZE   (1u << 20)
#define KORU_MAX_SLOT_COUNT  4096u
#define KORU_MAX_ARENA_BYTES (64ull << 20)
/* Bounded by the handle encoding: the index is 16 bits. */
#define KORU_MAX_HANDLES 4096u

/* What handle_count == 0 means at SETUP. */
#define KORU_DEFAULT_HANDLES 64u

/* Cap on a DELAY_NS delay: one hour. Unbounded, a delay pins a CQ reservation
 * for as long as it lasts. */
#define KORU_MAX_DELAY_NS 3600000000000ull

/* Ring configuration, for SETUP and GET_PARAMS. Field order gives natural
 * alignment with no padding: eight uint32_t, then the uint64_t at offset 32. */
struct koru_params {
    uint32_t magic;           /* in: must be KORU_MAGIC, else EPROTO */
    uint32_t abi_version;     /* in: must be KORU_ABI_VERSION, else EPROTO */
    uint32_t flags;           /* in: outside KORU_SETUP_FLAGS_ALL is EINVAL */
    uint32_t sq_entries;      /* in/out: submission depth, non-zero */
    uint32_t cq_entries;      /* in/out: completion depth; 0 means sq_entries */
    uint32_t slot_size;       /* in/out: one slot, a multiple of PAGE_SIZE */
    uint32_t slot_count;      /* in/out: number of arena slots */
    uint32_t configured;      /* out: non-zero once SETUP has succeeded */
    uint64_t features;        /* out: capability bits; none defined yet */
    uint64_t arena_size;      /* out: exact mmap length, slot_size*slot_count */
    uint32_t max_sq_entries;  /* out: cap */
    uint32_t max_cq_entries;  /* out: cap */
    uint32_t max_slot_size;   /* out: cap */
    uint32_t max_slot_count;  /* out: cap */
    uint64_t max_arena_bytes; /* out: cap on arena_size */
    uint32_t handle_count;    /* in/out: 0 means KORU_DEFAULT_HANDLES */
    uint32_t max_handles;     /* out: cap on handle_count */
    uint64_t max_delay_ns;    /* out: cap on a DELAY_NS delay */
    uint64_t reserved[2];     /* in: must be zero */
};

/* A submission queue entry. Fields an opcode does not read must be zero.
 *
 * off is a file offset on READ and WRITE, a within-slot offset on OPEN,
 * CHECKSUM and STAT, nanoseconds on DELAY_NS, the target's user_data on CANCEL,
 * a file descriptor on ADOPT_FD, and zero on NOP and CLOSE. */
struct koru_sqe {
    uint8_t opcode;     /* one of the KORU_OP_* constants */
    uint8_t flags;      /* outside KORU_SQE_FLAGS_ALL completes EINVAL */
    uint16_t rsvd0;     /* must be zero */
    uint32_t len;       /* opcode-specific length */
    uint64_t off;       /* opcode-specific offset; see above */
    uint64_t user_data; /* echoed into the CQE; opaque */
    uint32_t slot;      /* arena slot index; buffers are never addresses */
    uint32_t handle;    /* index low, generation high; OPEN flags on OPEN */
};

/* A completion queue entry. 32 bytes, so one never straddles a cache line. */
struct koru_cqe {
    uint64_t user_data; /* copied from the SQE that produced this */
    int64_t res;        /* >= 0 on success, negative errno on failure */
    uint32_t flags;     /* currently always zero; see KORU_CQE_F_MORE */
    uint32_t rsvd0;     /* must be zero */
    uint64_t extra;     /* opcode-specific; STAT's filled-field mask */
};

/* The ENTER ioctl argument. */
struct koru_enter {
    uint64_t sq_addr;      /* in: array of at least to_submit koru_sqe */
    uint64_t cq_addr;      /* in: array of at least cq_space koru_cqe */
    uint64_t timeout_ns;   /* in: relative MONOTONIC cap; 0 means no cap */
    uint32_t to_submit;    /* in: number of SQEs to consume */
    uint32_t cq_space;     /* in: capacity of the CQE array, in entries */
    uint32_t min_complete; /* in: completions to wait for; 0 returns at once */
    uint32_t flags;        /* in: outside KORU_ENTER_FLAGS_ALL fails the ioctl */
    uint32_t completed;    /* out: CQEs written */
    uint32_t submitted;    /* out: SQEs consumed; written on every path */
    uint64_t reserved[2];  /* in: must be zero */
};

/* Size alone would not catch two fields being swapped, so assert every
 * offset. The kernel file and rust/sys/src/abi.rs assert the same set. */

KORU_STATIC_ASSERT(sizeof(struct koru_params) == 104, "params size");
KORU_STATIC_ASSERT(KORU_ALIGNOF(struct koru_params) == 8, "params align");
KORU_STATIC_ASSERT(offsetof(struct koru_params, magic) == 0, "params.magic");
KORU_STATIC_ASSERT(offsetof(struct koru_params, abi_version) == 4, "params.abi_version");
KORU_STATIC_ASSERT(offsetof(struct koru_params, flags) == 8, "params.flags");
KORU_STATIC_ASSERT(offsetof(struct koru_params, sq_entries) == 12, "params.sq_entries");
KORU_STATIC_ASSERT(offsetof(struct koru_params, cq_entries) == 16, "params.cq_entries");
KORU_STATIC_ASSERT(offsetof(struct koru_params, slot_size) == 20, "params.slot_size");
KORU_STATIC_ASSERT(offsetof(struct koru_params, slot_count) == 24, "params.slot_count");
KORU_STATIC_ASSERT(offsetof(struct koru_params, configured) == 28, "params.configured");
KORU_STATIC_ASSERT(offsetof(struct koru_params, features) == 32, "params.features");
KORU_STATIC_ASSERT(offsetof(struct koru_params, arena_size) == 40, "params.arena_size");
KORU_STATIC_ASSERT(offsetof(struct koru_params, max_sq_entries) == 48, "params.max_sq_entries");
KORU_STATIC_ASSERT(offsetof(struct koru_params, max_cq_entries) == 52, "params.max_cq_entries");
KORU_STATIC_ASSERT(offsetof(struct koru_params, max_slot_size) == 56, "params.max_slot_size");
KORU_STATIC_ASSERT(offsetof(struct koru_params, max_slot_count) == 60, "params.max_slot_count");
KORU_STATIC_ASSERT(offsetof(struct koru_params, max_arena_bytes) == 64, "params.max_arena_bytes");
KORU_STATIC_ASSERT(offsetof(struct koru_params, handle_count) == 72, "params.handle_count");
KORU_STATIC_ASSERT(offsetof(struct koru_params, max_handles) == 76, "params.max_handles");
KORU_STATIC_ASSERT(offsetof(struct koru_params, max_delay_ns) == 80, "params.max_delay_ns");
KORU_STATIC_ASSERT(offsetof(struct koru_params, reserved) == 88, "params.reserved");

KORU_STATIC_ASSERT(sizeof(struct koru_sqe) == 32, "sqe size");
KORU_STATIC_ASSERT(KORU_ALIGNOF(struct koru_sqe) == 8, "sqe align");
KORU_STATIC_ASSERT(offsetof(struct koru_sqe, opcode) == 0, "sqe.opcode");
KORU_STATIC_ASSERT(offsetof(struct koru_sqe, flags) == 1, "sqe.flags");
KORU_STATIC_ASSERT(offsetof(struct koru_sqe, rsvd0) == 2, "sqe.rsvd0");
KORU_STATIC_ASSERT(offsetof(struct koru_sqe, len) == 4, "sqe.len");
KORU_STATIC_ASSERT(offsetof(struct koru_sqe, off) == 8, "sqe.off");
KORU_STATIC_ASSERT(offsetof(struct koru_sqe, user_data) == 16, "sqe.user_data");
KORU_STATIC_ASSERT(offsetof(struct koru_sqe, slot) == 24, "sqe.slot");
KORU_STATIC_ASSERT(offsetof(struct koru_sqe, handle) == 28, "sqe.handle");

KORU_STATIC_ASSERT(sizeof(struct koru_cqe) == 32, "cqe size");
KORU_STATIC_ASSERT(KORU_ALIGNOF(struct koru_cqe) == 8, "cqe align");
KORU_STATIC_ASSERT(offsetof(struct koru_cqe, user_data) == 0, "cqe.user_data");
KORU_STATIC_ASSERT(offsetof(struct koru_cqe, res) == 8, "cqe.res");
KORU_STATIC_ASSERT(offsetof(struct koru_cqe, flags) == 16, "cqe.flags");
KORU_STATIC_ASSERT(offsetof(struct koru_cqe, rsvd0) == 20, "cqe.rsvd0");
KORU_STATIC_ASSERT(offsetof(struct koru_cqe, extra) == 24, "cqe.extra");

KORU_STATIC_ASSERT(sizeof(struct koru_enter) == 64, "enter size");
KORU_STATIC_ASSERT(KORU_ALIGNOF(struct koru_enter) == 8, "enter align");
KORU_STATIC_ASSERT(offsetof(struct koru_enter, sq_addr) == 0, "enter.sq_addr");
KORU_STATIC_ASSERT(offsetof(struct koru_enter, cq_addr) == 8, "enter.cq_addr");
KORU_STATIC_ASSERT(offsetof(struct koru_enter, timeout_ns) == 16, "enter.timeout_ns");
KORU_STATIC_ASSERT(offsetof(struct koru_enter, to_submit) == 24, "enter.to_submit");
KORU_STATIC_ASSERT(offsetof(struct koru_enter, cq_space) == 28, "enter.cq_space");
KORU_STATIC_ASSERT(offsetof(struct koru_enter, min_complete) == 32, "enter.min_complete");
KORU_STATIC_ASSERT(offsetof(struct koru_enter, flags) == 36, "enter.flags");
/* completed precedes submitted. */
KORU_STATIC_ASSERT(offsetof(struct koru_enter, completed) == 40, "enter.completed");
KORU_STATIC_ASSERT(offsetof(struct koru_enter, submitted) == 44, "enter.submitted");
KORU_STATIC_ASSERT(offsetof(struct koru_enter, reserved) == 48, "enter.reserved");

/* The ioctl numbers embed the struct sizes, so a layout change moves them.
 * The literals are what koru_sys::sys's own canary asserts. */
#define KORU_IOC_SETUP      _IOWR(KORU_IOC_TYPE, KORU_NR_SETUP, struct koru_params)
#define KORU_IOC_GET_PARAMS _IOR(KORU_IOC_TYPE, KORU_NR_GET_PARAMS, struct koru_params)
#define KORU_IOC_ENTER      _IOWR(KORU_IOC_TYPE, KORU_NR_ENTER, struct koru_enter)

KORU_STATIC_ASSERT(KORU_IOC_SETUP == 0xc0686b00u, "ioctl SETUP");
KORU_STATIC_ASSERT(KORU_IOC_GET_PARAMS == 0x80686b01u, "ioctl GET_PARAMS");
KORU_STATIC_ASSERT(KORU_IOC_ENTER == 0xc0406b02u, "ioctl ENTER");

/* Opcodes. ABI numbers; unimplemented ones complete EINVAL. */
#define KORU_OP_NOP      0
#define KORU_OP_DELAY_NS 1
#define KORU_OP_OPEN     2
#define KORU_OP_READ     3
#define KORU_OP_CLOSE    4
#define KORU_OP_CANCEL   5
#define KORU_OP_CHECKSUM 6
#define KORU_OP_WRITE    7
#define KORU_OP_ADOPT_FD 8
#define KORU_OP_POLL_ADD 9
/* Stat handle into slot `slot` at slot offset `off`, a multiple of 8. `len` is
 * the caller's buffer size and doubles as version negotiation: the kernel
 * writes min(len, sizeof(struct koru_stat)) bytes and returns that in res.
 * extra is the KORU_STAT_* mask of fields the filesystem reported. */
#define KORU_OP_STAT 10
/* Path operations. Every one names its path the way OPEN does: len bytes at off
 * in slot `slot`, with handle zero. An argument that does not fit in the SQE
 * follows the path in the same slot, at the first 8-aligned offset at or after
 * its end.
 *
 * TRUNCATE's argument is a uint64_t new length and res is 0; UTIMES' is a
 * struct koru_times and res is 0; READLINK has none and its target replaces the
 * path it was given, NUL-terminated at off, with res the length without the
 * NUL. TRUNCATE and UTIMES follow a final symlink; READLINK does not. */
#define KORU_OP_TRUNCATE 11
#define KORU_OP_UTIMES   12
#define KORU_OP_READLINK 13
/* Stat by path. No argument: the struct koru_stat replaces the path it was
 * given, at off in the same slot, and res is its size. off must be a multiple
 * of eight and the whole struct must fit in the rest of the slot, else EINVAL.
 * extra carries the KORU_STAT_* mask, as on STAT. Follows a final symlink.
 * len is the path's length here, so there is no version negotiation as on
 * STAT: a later field comes out of koru_stat.reserved, which fixes the struct's
 * size for every binary at this ABI version. */
#define KORU_OP_STATX_AT 14

KORU_STATIC_ASSERT(KORU_OP_NOP == 0, "op NOP");
KORU_STATIC_ASSERT(KORU_OP_DELAY_NS == 1, "op DELAY_NS");
KORU_STATIC_ASSERT(KORU_OP_OPEN == 2, "op OPEN");
KORU_STATIC_ASSERT(KORU_OP_READ == 3, "op READ");
KORU_STATIC_ASSERT(KORU_OP_CLOSE == 4, "op CLOSE");
KORU_STATIC_ASSERT(KORU_OP_CANCEL == 5, "op CANCEL");
KORU_STATIC_ASSERT(KORU_OP_CHECKSUM == 6, "op CHECKSUM");
KORU_STATIC_ASSERT(KORU_OP_WRITE == 7, "op WRITE");
KORU_STATIC_ASSERT(KORU_OP_ADOPT_FD == 8, "op ADOPT_FD");
KORU_STATIC_ASSERT(KORU_OP_POLL_ADD == 9, "op POLL_ADD");
KORU_STATIC_ASSERT(KORU_OP_STAT == 10, "op STAT");
KORU_STATIC_ASSERT(KORU_OP_TRUNCATE == 11, "op TRUNCATE");
KORU_STATIC_ASSERT(KORU_OP_UTIMES == 12, "op UTIMES");
KORU_STATIC_ASSERT(KORU_OP_READLINK == 13, "op READLINK");
KORU_STATIC_ASSERT(KORU_OP_STATX_AT == 14, "op STATX_AT");

/* Any bit set in an SQE's flags is rejected. */
#define KORU_SQE_FLAGS_ALL 0u

/* Poll events, carried in a POLL_ADD SQE's len and returned in res. koru's own
 * bit values, like the open flags. ERR and HUP are reported whether or not
 * they were asked for. */
#define KORU_POLL_IN    (1u << 0) /* readable, or end of file on a stream */
#define KORU_POLL_OUT   (1u << 1) /* writable */
#define KORU_POLL_PRI   (1u << 2) /* out-of-band data */
#define KORU_POLL_RDHUP (1u << 3) /* the peer closed its writing half */
#define KORU_POLL_ERR   (1u << 4)
#define KORU_POLL_HUP   (1u << 5)

/* Any bit outside this completes EINVAL, and so does an empty mask. */
#define KORU_POLL_EVENTS_ALL                                                                       \
    (KORU_POLL_IN | KORU_POLL_OUT | KORU_POLL_PRI | KORU_POLL_RDHUP | KORU_POLL_ERR |               \
     KORU_POLL_HUP)

/* Open flags, carried in an OPEN SQE's handle field. koru's own bit values,
 * not the host O_*; the kernel translates. */
#define KORU_O_ACCMODE   0x3u /* access mode: the low two bits; 3 is invalid */
#define KORU_O_RDONLY    0u
#define KORU_O_WRONLY    1u
#define KORU_O_RDWR      2u
#define KORU_O_NOFOLLOW  (1u << 2) /* ELOOP rather than following a symlink */
#define KORU_O_DIRECTORY (1u << 3) /* ENOTDIR unless the path is a directory */
/* Open without blocking, and make READ and WRITE answer EAGAIN rather than
 * wait. Required to READ or WRITE anything but a regular file. koru never sets
 * or clears O_NONBLOCK on a file it did not open. */
#define KORU_O_NONBLOCK (1u << 4)

/* Any bit outside this completes EINVAL. No O_CREAT: no field carries a
 * creation mode. */
#define KORU_OPEN_FLAGS_ALL                                                                        \
    (KORU_O_ACCMODE | KORU_O_NOFOLLOW | KORU_O_DIRECTORY | KORU_O_NONBLOCK)

/* Multishot bit, reserved and never set. */
#define KORU_CQE_F_MORE (1u << 0)

/* File type, the top bits of koru_stat.mode. Unlike the open flags, these
 * values are the same on every Linux architecture, so they pass through. */
#define KORU_S_IFMT   0170000u
#define KORU_S_IFIFO  0010000u
#define KORU_S_IFCHR  0020000u
#define KORU_S_IFDIR  0040000u
#define KORU_S_IFBLK  0060000u
#define KORU_S_IFREG  0100000u
#define KORU_S_IFLNK  0120000u
#define KORU_S_IFSOCK 0140000u

/* Which koru_stat fields the kernel filled, returned in cqe.extra. koru's own
 * bits, one per field and in this struct's field order, not statx's. blksize,
 * dev and rdev have no statx bit because the VFS always fills them, and they
 * get one here so the mask describes the whole struct. */
#define KORU_STAT_INO     (1ull << 0)
#define KORU_STAT_SIZE    (1ull << 1)
#define KORU_STAT_BLOCKS  (1ull << 2)
#define KORU_STAT_BLKSIZE (1ull << 3)
#define KORU_STAT_NLINK   (1ull << 4)
#define KORU_STAT_TYPE    (1ull << 5)
#define KORU_STAT_MODE    (1ull << 6)
#define KORU_STAT_UID     (1ull << 7)
#define KORU_STAT_GID     (1ull << 8)
#define KORU_STAT_DEV     (1ull << 9)
#define KORU_STAT_RDEV    (1ull << 10)
#define KORU_STAT_ATIME   (1ull << 11)
#define KORU_STAT_MTIME   (1ull << 12)
#define KORU_STAT_CTIME   (1ull << 13)
#define KORU_STAT_BTIME   (1ull << 14)

/* Everything STAT can report. A filesystem may report less; never more. */
#define KORU_STAT_ALL                                                                              \
    (KORU_STAT_INO | KORU_STAT_SIZE | KORU_STAT_BLOCKS | KORU_STAT_BLKSIZE | KORU_STAT_NLINK |     \
     KORU_STAT_TYPE | KORU_STAT_MODE | KORU_STAT_UID | KORU_STAT_GID | KORU_STAT_DEV |             \
     KORU_STAT_RDEV | KORU_STAT_ATIME | KORU_STAT_MTIME | KORU_STAT_CTIME | KORU_STAT_BTIME)

/* What STAT writes into the slot. 256 bytes, every field 64 bits, no padding.
 * Times are second-plus-nanosecond pairs and device numbers are explicit major
 * and minor, so nothing here is a kernel-internal encoding. */
struct koru_stat {
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;  /* 512-byte blocks allocated */
    uint64_t blksize; /* preferred I/O size */
    uint64_t nlink;
    uint64_t mode; /* file type in KORU_S_IFMT, permissions below it */
    uint64_t uid;  /* in the submitting task's user namespace */
    uint64_t gid;
    uint64_t dev_major;
    uint64_t dev_minor;
    uint64_t rdev_major;
    uint64_t rdev_minor;
    int64_t atime_sec; /* signed: a date before 1970 is a date */
    uint64_t atime_nsec;
    int64_t mtime_sec;
    uint64_t mtime_nsec;
    int64_t ctime_sec;
    uint64_t ctime_nsec;
    int64_t btime_sec; /* creation time; KORU_STAT_BTIME says if it is real */
    uint64_t btime_nsec;
    uint64_t reserved[12]; /* out: must read as zero */
};

KORU_STATIC_ASSERT(sizeof(struct koru_stat) == 256, "stat size");
KORU_STATIC_ASSERT(KORU_ALIGNOF(struct koru_stat) == 8, "stat align");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, ino) == 0, "stat.ino");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, size) == 8, "stat.size");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, blocks) == 16, "stat.blocks");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, blksize) == 24, "stat.blksize");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, nlink) == 32, "stat.nlink");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, mode) == 40, "stat.mode");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, uid) == 48, "stat.uid");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, gid) == 56, "stat.gid");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, dev_major) == 64, "stat.dev_major");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, dev_minor) == 72, "stat.dev_minor");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, rdev_major) == 80, "stat.rdev_major");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, rdev_minor) == 88, "stat.rdev_minor");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, atime_sec) == 96, "stat.atime_sec");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, atime_nsec) == 104, "stat.atime_nsec");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, mtime_sec) == 112, "stat.mtime_sec");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, mtime_nsec) == 120, "stat.mtime_nsec");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, ctime_sec) == 128, "stat.ctime_sec");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, ctime_nsec) == 136, "stat.ctime_nsec");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, btime_sec) == 144, "stat.btime_sec");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, btime_nsec) == 152, "stat.btime_nsec");
KORU_STATIC_ASSERT(offsetof(struct koru_stat, reserved) == 160, "stat.reserved");

/* Nanosecond sentinels, checked by the kernel itself. Same values everywhere,
 * so they pass through like the S_IF* ones. */
#define KORU_UTIME_NOW  ((int64_t)((1 << 30) - 1)) /* set this time to now */
#define KORU_UTIME_OMIT ((int64_t)((1 << 30) - 2)) /* leave this time alone */

/* UTIMES' argument, two (seconds, nanoseconds) pairs. 32 bytes, no padding. */
struct koru_times {
    int64_t atime_sec; /* signed: a date before 1970 is a date */
    int64_t atime_nsec; /* nanoseconds, or a KORU_UTIME_* sentinel */
    int64_t mtime_sec;
    int64_t mtime_nsec;
};

KORU_STATIC_ASSERT(sizeof(struct koru_times) == 32, "times size");
KORU_STATIC_ASSERT(KORU_ALIGNOF(struct koru_times) == 8, "times align");
KORU_STATIC_ASSERT(offsetof(struct koru_times, atime_sec) == 0, "times.atime_sec");
KORU_STATIC_ASSERT(offsetof(struct koru_times, atime_nsec) == 8, "times.atime_nsec");
KORU_STATIC_ASSERT(offsetof(struct koru_times, mtime_sec) == 16, "times.mtime_sec");
KORU_STATIC_ASSERT(offsetof(struct koru_times, mtime_nsec) == 24, "times.mtime_nsec");

/* Handle encoding: index in the low half, generation in the high half. A
 * valid handle is never 0, because the generation starts at 1. */
#define KORU_HANDLE_INDEX(h)     ((uint32_t)(h) & 0xffffu)
#define KORU_HANDLE_GEN(h)       ((uint32_t)(h) >> 16)
#define KORU_MAKE_HANDLE(i, gen) (((uint32_t)(i) & 0xffffu) | ((uint32_t)(gen) << 16))

#endif /* KORU_ABI_H */
