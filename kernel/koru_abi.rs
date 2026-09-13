// SPDX-License-Identifier: GPL-2.0

//! The koru wire format. **Canonical**; `cpp/include/koru_abi.h` mirrors
//! it and T14 diffs the two.
//!
//! Two rules hold everywhere: reserved fields must be zero and unknown flag bits
//! are rejected, and nothing has padding (`AsBytes` forbids uninitialised bytes).

use kernel::error::Error;
use kernel::ioctl::{_IOR, _IOWR};
use kernel::transmute::{AsBytes, FromBytes};

/// `EPROTO`: ABI mismatch, as opposed to `EINVAL` for a bad value.
/// Not in `kernel::error::code`, and the number varies by architecture.
pub(crate) fn eproto() -> Error {
    Error::from_errno(-(kernel::uapi::EPROTO as i32))
}

/// `ECANCELED`: the completion a cancelled op gets. Not in `error::code`.
pub(crate) fn ecanceled() -> Error {
    Error::from_errno(-(kernel::uapi::ECANCELED as i32))
}

/// `EALREADY`: the target was found but is already running. Not in `error::code`.
pub(crate) fn ealready() -> Error {
    Error::from_errno(-(kernel::uapi::EALREADY as i32))
}

/// `ELOOP`: adopting a koru descriptor into a koru ring. Not in `error::code`.
pub(crate) fn eloop() -> Error {
    Error::from_errno(-(kernel::uapi::ELOOP as i32))
}

/// `EOPNOTSUPP`: a poll whose file wants two waitqueues. `error::code` has
/// `ENOTSUPP`, which is a different number and is not a uapi errno.
pub(crate) fn eopnotsupp() -> Error {
    Error::from_errno(-(kernel::uapi::EOPNOTSUPP as i32))
}

/// `ENAMETOOLONG`: a `READLINK` answer longer than the slot has room for.
/// Not in `error::code`.
pub(crate) fn enametoolong() -> Error {
    Error::from_errno(-(kernel::uapi::ENAMETOOLONG as i32))
}

/// Set by userspace in [`KoruParams::magic`]. Spells "koru" little-endian.
pub(crate) const KORU_MAGIC: u32 = 0x7572_6f6b;

/// Bumped on any incompatible change to this file.
pub(crate) const KORU_ABI_VERSION: u32 = 1;

/// ioctl type byte. Listed in `Documentation/userspace-api/ioctl/ioctl-number.rst`
/// as conflicting: `linux/spi/spidev.h` 00-0F and `video/kyro.h` 00-05 overlap the
/// command numbers below. Harmless — the fd identifies the driver.
pub(crate) const KORU_IOC_TYPE: u32 = 'k' as u32;

/// Command numbers. Dispatch uses these, not the whole ioctl number, to keep
/// `ENOTTY` (no such command) distinct from `EPROTO` (wrong struct size).
pub(crate) const KORU_NR_SETUP: u32 = 0x00;
pub(crate) const KORU_NR_GET_PARAMS: u32 = 0x01;
pub(crate) const KORU_NR_ENTER: u32 = 0x02;

// Unused here because dispatch is by command number, but these are what
// userspace passes and what the C mirror must reproduce.

/// Configure the ring. Callable exactly once per fd, before `mmap`.
///
/// Bidirectional: userspace supplies its request, the kernel writes back the
/// effective values and the caps.
#[expect(dead_code)]
pub(crate) const KORU_IOC_SETUP: u32 = _IOWR::<KoruParams>(KORU_IOC_TYPE, KORU_NR_SETUP);

/// Read the current parameters. Legal before `SETUP`, in which case only the
/// caps and the ABI fields are meaningful and `configured` is 0.
#[expect(dead_code)]
pub(crate) const KORU_IOC_GET_PARAMS: u32 =
    _IOR::<KoruParams>(KORU_IOC_TYPE, KORU_NR_GET_PARAMS);

/// Submit SQEs and reap CQEs. Returns the count of SQEs **consumed**, never the
/// completion count (E1); CQEs written go in [`KoruEnter::completed`].
#[expect(dead_code)]
pub(crate) const KORU_IOC_ENTER: u32 = _IOWR::<KoruEnter>(KORU_IOC_TYPE, KORU_NR_ENTER);

/// No setup flags are defined yet, so any bit set is rejected.
pub(crate) const KORU_SETUP_FLAGS_ALL: u32 = 0;

/// No `ENTER` flags are defined yet, so any bit set is rejected.
pub(crate) const KORU_ENTER_FLAGS_ALL: u32 = 0;

/// Caps, reported in every [`KoruParams`]. A request above one is rejected,
/// not clamped. Reported rather than fixed in the header, so raising one is not
/// an ABI change. The arena is unaccounted memory, so its cap is load-bearing.
pub(crate) const KORU_MAX_SQ_ENTRIES: u32 = 4096;
pub(crate) const KORU_MAX_CQ_ENTRIES: u32 = 8192;
pub(crate) const KORU_MAX_SLOT_SIZE: u32 = 1 << 20;
pub(crate) const KORU_MAX_SLOT_COUNT: u32 = 4096;
pub(crate) const KORU_MAX_ARENA_BYTES: u64 = 64 << 20;
/// Bounded by the handle encoding: the index is 16 bits.
pub(crate) const KORU_MAX_HANDLES: u32 = 4096;

/// What `handle_count == 0` means at `SETUP`.
pub(crate) const KORU_DEFAULT_HANDLES: u32 = 64;

/// Cap on a `DELAY_NS` delay: one hour. Unbounded, a delay pins a CQ
/// reservation for as long as it lasts.
pub(crate) const KORU_MAX_DELAY_NS: u64 = 3_600_000_000_000;

/// Ring configuration, for `SETUP` and `GET_PARAMS`. Field order gives natural
/// alignment with no padding: eight `u32`, then the `u64`s at offset 32.
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub(crate) struct KoruParams {
    /// in: must be [`KORU_MAGIC`], else `EPROTO`.
    pub(crate) magic: u32,
    /// in: must be [`KORU_ABI_VERSION`], else `EPROTO`.
    pub(crate) abi_version: u32,
    /// in: setup flags. Any bit outside [`KORU_SETUP_FLAGS_ALL`] is `EINVAL`.
    pub(crate) flags: u32,
    /// in/out: submission queue depth. Must be non-zero.
    pub(crate) sq_entries: u32,
    /// in/out: completion queue depth; 0 means "same as `sq_entries`". Must end
    /// up `>= sq_entries`, or admission control cannot reserve a slot per SQE.
    pub(crate) cq_entries: u32,
    /// in/out: size of one arena slot, in bytes.
    pub(crate) slot_size: u32,
    /// in/out: number of arena slots.
    pub(crate) slot_count: u32,
    /// out: non-zero once `SETUP` has succeeded on this fd.
    pub(crate) configured: u32,

    /// out: capability bits. None defined yet.
    pub(crate) features: u64,
    /// out: exact length to pass to `mmap`, `slot_size * slot_count`.
    pub(crate) arena_size: u64,
    /// out: caps. See the `KORU_MAX_*` constants.
    pub(crate) max_sq_entries: u32,
    pub(crate) max_cq_entries: u32,
    pub(crate) max_slot_size: u32,
    pub(crate) max_slot_count: u32,
    /// out: cap on `arena_size`.
    pub(crate) max_arena_bytes: u64,
    /// in/out: handle table size; 0 means [`KORU_DEFAULT_HANDLES`]. Carved out
    /// of the old `reserved[3]` with `max_handles`.
    pub(crate) handle_count: u32,
    /// out: cap on `handle_count`.
    pub(crate) max_handles: u32,
    /// out: cap on a `DELAY_NS` delay, in nanoseconds.
    pub(crate) max_delay_ns: u64,
    /// in: must be zero.
    pub(crate) reserved: [u64; 2],
}

// 104 = eight u32 at 0..32, then u64-aligned fields; a multiple of 8, so no
// trailing padding.
kernel::static_assert!(core::mem::size_of::<KoruParams>() == 104);
kernel::static_assert!(core::mem::align_of::<KoruParams>() == 8);
// The two fields carved out of the old `reserved[3]`, and where that leaves it.
kernel::static_assert!(core::mem::offset_of!(KoruParams, handle_count) == 72);
kernel::static_assert!(core::mem::offset_of!(KoruParams, max_handles) == 76);
kernel::static_assert!(core::mem::offset_of!(KoruParams, max_delay_ns) == 80);
kernel::static_assert!(core::mem::offset_of!(KoruParams, reserved) == 88);

// SAFETY: `repr(C)`, unsigned integers only, so every bit pattern is valid. No
// interior mutability.
unsafe impl FromBytes for KoruParams {}

// SAFETY: `repr(C)` and padding-free per the assertions above, so no
// uninitialised bytes. No interior mutability.
unsafe impl AsBytes for KoruParams {}

// ---------------------------------------------------------------------------
// Submission and completion
// ---------------------------------------------------------------------------

/// Opcodes. These numbers are ABI; unimplemented ones complete with `EINVAL`.
pub(crate) const KORU_OP_NOP: u8 = 0; // T4
/// Delay for `off` nanoseconds. `len`, `slot` and `handle` must be zero.
pub(crate) const KORU_OP_DELAY_NS: u8 = 1; // T5
/// Open the path held in `len` bytes at `off` in slot `slot`. `handle` carries
/// the open flags. On success `res` is the new handle, always positive.
pub(crate) const KORU_OP_OPEN: u8 = 2; // T9
/// Read `len` bytes from file offset `off` of `handle` into slot `slot`, at
/// slot offset 0. `res` is the count actually read: short at EOF, 0 at or past
/// it. Regular files, or anything opened `KORU_O_NONBLOCK`.
pub(crate) const KORU_OP_READ: u8 = 3; // T10
/// Retire the handle in `handle`. `len`, `off` and `slot` must be zero.
pub(crate) const KORU_OP_CLOSE: u8 = 4; // T9
/// Cancel the in-flight op whose `user_data` is `off`. `len`, `slot` and
/// `handle` must be zero. `res` is 0 when cancelled, `-ENOENT` when no such op
/// is in flight, `-EALREADY` when it is already running. The target always gets
/// its own CQE; the order of the two is unspecified.
pub(crate) const KORU_OP_CANCEL: u8 = 5; // T11
/// FNV-1a over `len` bytes at `off` in slot `slot`, returned in `res`.
/// `handle` must be zero. Scaffolding for the arena; see doc/Notes.md.
pub(crate) const KORU_OP_CHECKSUM: u8 = 6; // T7
/// Write `len` bytes from slot `slot`, at slot offset 0, to file offset `off`
/// of `handle`. `res` is the count written; short is a result, not an error.
/// Regular files, or anything opened `KORU_O_NONBLOCK`. A non-zero `off` needs
/// a seekable file, else `EINVAL`; on an `O_APPEND` handle the kernel appends
/// and `off` is ignored.
pub(crate) const KORU_OP_WRITE: u8 = 7; // T17
/// Adopt the already-open descriptor in `off` and return a handle for it, so
/// stdin, stdout and stderr can reach the ring. `len`, `slot` and `handle` must
/// be zero, and `off` must be at most `i32::MAX`.
///
/// **Grants no authority the submitting task does not already hold**: no
/// permission check happens here, because the task already has the descriptor.
/// Adopting any koru descriptor is `ELOOP` — it would make the ring
/// unreachable by `release` and the module unloadable.
pub(crate) const KORU_OP_ADOPT_FD: u8 = 8; // T19
/// Wait for one of the events in `len` on `handle`, once. `off` and `slot`
/// must be zero. `res` is the event mask that fired, always non-negative and
/// never confusable with an errno.
///
/// Single-shot: admission control reserves a CQ slot per consumed SQE, so a
/// multishot poll would have no home for its second completion.
///
/// A file on no waitqueue — every regular file, which has no `poll` method —
/// completes at once with its default mask, and `res` is 0 where none of the
/// asked-for events are in it. Nothing could ever wake such a poll, so waiting
/// would pin a CQ reservation for the life of the ring.
pub(crate) const KORU_OP_POLL_ADD: u8 = 9; // T22
/// Stat `handle` into slot `slot` at slot offset `off`, which must be a
/// multiple of 8. `len` is the caller's buffer size and doubles as version
/// negotiation: the kernel writes `min(len, sizeof(KoruStat))` bytes and
/// returns that count in `res`. `extra` is the [`KORU_STAT_*`](KORU_STAT_INO)
/// mask of fields the filesystem actually reported.
pub(crate) const KORU_OP_STAT: u8 = 10; // T23

// Path operations. Every one of them names its path the way `OPEN` does:
// `len` bytes at `off` in slot `slot`, with `handle` zero. An argument that
// does not fit in the SQE follows the path in the same slot, at the first
// 8-aligned offset at or after its end.

/// Set the length of the file the path names. The argument is a `u64`, the new
/// length; `res` is 0. Follows a final symlink, as `truncate(2)` does.
pub(crate) const KORU_OP_TRUNCATE: u8 = 11; // T24
/// Set the access and modification times of the file the path names. The
/// argument is a [`KoruTimes`]; `res` is 0. Follows a final symlink.
pub(crate) const KORU_OP_UTIMES: u8 = 12; // T24
/// Read the symlink the path names. No argument: the target **replaces the
/// path it was given**, NUL-terminated, at `off` in the same slot, and `res` is
/// its length without the NUL. Does not follow a final symlink, which is the
/// entire point. `ENAMETOOLONG` if it does not fit in the rest of the slot —
/// truncating a path silently is how a wrong path gets used.
pub(crate) const KORU_OP_READLINK: u8 = 13; // T24
/// Stat the file the path names. No argument: the [`KoruStat`] **replaces the
/// path it was given**, at `off` in the same slot, and `res` is its size. `off`
/// must be a multiple of eight and the whole struct must fit in the rest of the
/// slot, else `EINVAL`. `extra` carries the [`KORU_STAT_*`](KORU_STAT_INO)
/// mask, as on `STAT`. Follows a final symlink, as `stat(2)` does.
///
/// `len` is the path's length here, so there is no version negotiation as on
/// `STAT`: a later field comes out of [`KoruStat::reserved`], which is what
/// makes the struct's size the same for every binary at this ABI version.
pub(crate) const KORU_OP_STATX_AT: u8 = 14; // T25

/// Any bit set is rejected.
pub(crate) const KORU_SQE_FLAGS_ALL: u8 = 0;

// Poll events, carried in a `POLL_ADD` SQE's `len` and returned in `res`.
// koru's own bit values, like the open flags: the host `EPOLL*` constants are
// `__force`-cast and vary in spelling, and this way `res` stays small.

/// Readable, or end of file on a stream.
pub(crate) const KORU_POLL_IN: u32 = 1 << 0;
/// Writable.
pub(crate) const KORU_POLL_OUT: u32 = 1 << 1;
/// Out-of-band data.
pub(crate) const KORU_POLL_PRI: u32 = 1 << 2;
/// The peer closed its writing half.
pub(crate) const KORU_POLL_RDHUP: u32 = 1 << 3;
/// Error. Reported whether or not it was asked for.
pub(crate) const KORU_POLL_ERR: u32 = 1 << 4;
/// Hang-up. Reported whether or not it was asked for.
pub(crate) const KORU_POLL_HUP: u32 = 1 << 5;

/// Any bit outside this completes with `EINVAL`, and so does an empty mask:
/// a poll for nothing can only ever report an error.
pub(crate) const KORU_POLL_EVENTS_ALL: u32 =
    KORU_POLL_IN | KORU_POLL_OUT | KORU_POLL_PRI | KORU_POLL_RDHUP | KORU_POLL_ERR | KORU_POLL_HUP;

// Open flags, carried in an `OPEN` SQE's `handle` field. koru's own bit values,
// not the host `O_*` constants, which vary by architecture. The kernel
// translates.

/// Access mode: the low two bits. Mode 3 is invalid.
pub(crate) const KORU_O_ACCMODE: u32 = 0x3;
#[expect(dead_code)]
pub(crate) const KORU_O_RDONLY: u32 = 0;
pub(crate) const KORU_O_WRONLY: u32 = 1;
pub(crate) const KORU_O_RDWR: u32 = 2;

/// Fail with `ELOOP` rather than following a final symlink.
pub(crate) const KORU_O_NOFOLLOW: u32 = 1 << 2;
/// Fail with `ENOTDIR` unless the path names a directory.
pub(crate) const KORU_O_DIRECTORY: u32 = 1 << 3;
/// Open without blocking, and make `READ` and `WRITE` answer `EAGAIN` rather
/// than wait. Required to `READ` or `WRITE` anything but a regular file: a
/// blocking transfer in a kworker cannot be interrupted.
///
/// **koru never sets or clears `O_NONBLOCK` on a file it did not open.** An
/// adopted descriptor's `struct file` is shared with the rest of the process,
/// so flipping it on stdin would change behaviour for every other holder.
pub(crate) const KORU_O_NONBLOCK: u32 = 1 << 4;

/// Any bit outside this completes with `EINVAL`. No `O_CREAT`: there is no
/// field for a creation mode.
pub(crate) const KORU_OPEN_FLAGS_ALL: u32 =
    KORU_O_ACCMODE | KORU_O_NOFOLLOW | KORU_O_DIRECTORY | KORU_O_NONBLOCK;

/// Multishot bit, reserved and never set: admission control forecloses multishot.
#[expect(dead_code)]
pub(crate) const KORU_CQE_F_MORE: u32 = 1 << 0;

/// A submission queue entry. 32 bytes, no padding. Fields an opcode does not
/// read must be zero, so they stay available for a later meaning.
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub(crate) struct Sqe {
    /// One of the `KORU_OP_*` constants.
    pub(crate) opcode: u8,
    /// Any bit outside [`KORU_SQE_FLAGS_ALL`] completes with `EINVAL`.
    pub(crate) flags: u8,
    /// Must be zero.
    pub(crate) rsvd0: u16,
    /// Opcode-specific length.
    pub(crate) len: u32,
    /// Opcode-specific offset. A file offset on `READ` and `WRITE`, a
    /// within-slot offset on `OPEN`, `CHECKSUM` and `STAT`, nanoseconds on
    /// `DELAY_NS`, the target's `user_data` on `CANCEL`, a file descriptor on
    /// `ADOPT_FD`. `NOP` and `CLOSE` want zero.
    pub(crate) off: u64,
    /// Echoed into the CQE. Opaque; userspace packs (slab index, generation).
    pub(crate) user_data: u64,
    /// Arena slot index. Buffers are named by index, never by address.
    pub(crate) slot: u32,
    /// Handle from `OPEN`: index in the low half, generation in the high half.
    /// A generation is never 0, so a valid handle is never 0.
    ///
    /// On an `OPEN` SQE this carries the `KORU_O_*` flags instead.
    pub(crate) handle: u32,
}

/// A completion queue entry. 32 bytes, so one never straddles a cache line.
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub(crate) struct Cqe {
    /// Copied from the SQE that produced this completion.
    pub(crate) user_data: u64,
    /// `>= 0` on success, negative errno on failure. 64-bit so large reads and
    /// offsets stay representable.
    pub(crate) res: i64,
    /// Currently always zero. See [`KORU_CQE_F_MORE`].
    pub(crate) flags: u32,
    /// Must be zero.
    pub(crate) rsvd0: u32,
    /// Opcode-specific extra result, zero on every opcode that defines none.
    /// `STAT` puts the mask of fields it filled here.
    pub(crate) extra: u64,
}

/// The `ENTER` ioctl argument. 64 bytes, no padding.
///
/// `sq_addr`/`cq_addr` are user addresses reached only by `copy_*_user`. The
/// never-dereference invariant is about operation buffers, which are named by
/// slot index and live in kernel-owned pages.
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub(crate) struct KoruEnter {
    /// in: address of an array of at least `to_submit` [`Sqe`].
    pub(crate) sq_addr: u64,
    /// in: address of an array of at least `cq_space` [`Cqe`].
    pub(crate) cq_addr: u64,
    /// in: relative MONOTONIC cap on the wait; 0 means no cap. Only consulted
    /// when `min_complete > 0`. Rounds to jiffy granularity.
    pub(crate) timeout_ns: u64,
    /// in: number of SQEs to consume.
    pub(crate) to_submit: u32,
    /// in: capacity of the CQE array, counted in entries, not bytes.
    pub(crate) cq_space: u32,
    /// in: completions to wait for. 0 returns immediately without waiting.
    pub(crate) min_complete: u32,
    /// in: any bit outside [`KORU_ENTER_FLAGS_ALL`] fails the ioctl.
    pub(crate) flags: u32,
    /// out: CQEs written. Separate from the ioctl return, which E1 reserves for
    /// SQEs consumed; the two differ when completions are left queued.
    pub(crate) completed: u32,
    /// out: SQEs consumed. Written on every path, error ones included, so an
    /// `EINTR` return still tells the caller what not to resubmit.
    pub(crate) submitted: u32,
    /// in: must be zero.
    pub(crate) reserved: [u64; 2],
}

// ---------------------------------------------------------------------------
// Stat
// ---------------------------------------------------------------------------

// File type, the top bits of [`KoruStat::mode`]. Unlike the open flags, these
// values are the same on every Linux architecture, so they are passed through.

#[expect(dead_code)]
pub(crate) const KORU_S_IFMT: u64 = 0o170000;
#[expect(dead_code)]
pub(crate) const KORU_S_IFIFO: u64 = 0o010000;
#[expect(dead_code)]
pub(crate) const KORU_S_IFCHR: u64 = 0o020000;
#[expect(dead_code)]
pub(crate) const KORU_S_IFDIR: u64 = 0o040000;
#[expect(dead_code)]
pub(crate) const KORU_S_IFBLK: u64 = 0o060000;
#[expect(dead_code)]
pub(crate) const KORU_S_IFREG: u64 = 0o100000;
#[expect(dead_code)]
pub(crate) const KORU_S_IFLNK: u64 = 0o120000;
#[expect(dead_code)]
pub(crate) const KORU_S_IFSOCK: u64 = 0o140000;

// Which [`KoruStat`] fields the kernel filled, returned in `Cqe::extra`.
// koru's own bits, one per field and in this struct's field order, not statx's:
// that mask names fields koru does not carry and misses three koru does.
// `blksize`, `dev` and `rdev` have no statx bit because the VFS always fills
// them, and they get one here so the mask describes the whole struct.

pub(crate) const KORU_STAT_INO: u64 = 1 << 0;
pub(crate) const KORU_STAT_SIZE: u64 = 1 << 1;
pub(crate) const KORU_STAT_BLOCKS: u64 = 1 << 2;
pub(crate) const KORU_STAT_BLKSIZE: u64 = 1 << 3;
pub(crate) const KORU_STAT_NLINK: u64 = 1 << 4;
pub(crate) const KORU_STAT_TYPE: u64 = 1 << 5;
pub(crate) const KORU_STAT_MODE: u64 = 1 << 6;
pub(crate) const KORU_STAT_UID: u64 = 1 << 7;
pub(crate) const KORU_STAT_GID: u64 = 1 << 8;
pub(crate) const KORU_STAT_DEV: u64 = 1 << 9;
pub(crate) const KORU_STAT_RDEV: u64 = 1 << 10;
pub(crate) const KORU_STAT_ATIME: u64 = 1 << 11;
pub(crate) const KORU_STAT_MTIME: u64 = 1 << 12;
pub(crate) const KORU_STAT_CTIME: u64 = 1 << 13;
pub(crate) const KORU_STAT_BTIME: u64 = 1 << 14;

/// Everything `STAT` can report. A filesystem may report less; never more.
#[expect(dead_code)]
pub(crate) const KORU_STAT_ALL: u64 = KORU_STAT_INO
    | KORU_STAT_SIZE
    | KORU_STAT_BLOCKS
    | KORU_STAT_BLKSIZE
    | KORU_STAT_NLINK
    | KORU_STAT_TYPE
    | KORU_STAT_MODE
    | KORU_STAT_UID
    | KORU_STAT_GID
    | KORU_STAT_DEV
    | KORU_STAT_RDEV
    | KORU_STAT_ATIME
    | KORU_STAT_MTIME
    | KORU_STAT_CTIME
    | KORU_STAT_BTIME;

/// What `STAT` writes into the slot. 256 bytes, every field 64 bits, no
/// padding.
///
/// Times are second-plus-nanosecond pairs and device numbers are explicit major
/// and minor, so nothing here is a kernel-internal encoding. `reserved` is
/// zeroed and is where a later field is carved out, as `handle_count` was.
///
/// The whole destination is written, `reserved` included: a partly filled
/// struct would make the caller read its own stale bytes as kernel-reported
/// values.
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub(crate) struct KoruStat {
    pub(crate) ino: u64,
    pub(crate) size: u64,
    /// 512-byte blocks allocated.
    pub(crate) blocks: u64,
    pub(crate) blksize: u64,
    pub(crate) nlink: u64,
    /// File type in [`KORU_S_IFMT`], permission bits below it.
    pub(crate) mode: u64,
    /// Translated into the *submitting* task's user namespace.
    pub(crate) uid: u64,
    pub(crate) gid: u64,
    pub(crate) dev_major: u64,
    pub(crate) dev_minor: u64,
    pub(crate) rdev_major: u64,
    pub(crate) rdev_minor: u64,
    /// Seconds since the epoch; signed, because a date before 1970 is a date.
    pub(crate) atime_sec: i64,
    pub(crate) atime_nsec: u64,
    pub(crate) mtime_sec: i64,
    pub(crate) mtime_nsec: u64,
    pub(crate) ctime_sec: i64,
    pub(crate) ctime_nsec: u64,
    /// Creation time. Absent from most filesystems; [`KORU_STAT_BTIME`] says.
    pub(crate) btime_sec: i64,
    pub(crate) btime_nsec: u64,
    /// out: must read as zero.
    pub(crate) reserved: [u64; 12],
}

// ---------------------------------------------------------------------------
// Times
// ---------------------------------------------------------------------------

// Nanosecond sentinels, checked by `vfs_utimes` itself. Same values on every
// architecture, so they pass through like the `S_IF*` ones.

/// Set this timestamp to now, ignoring the seconds field. `vfs_utimes` reads
/// it, so the kernel side never names it.
#[expect(dead_code)]
pub(crate) const KORU_UTIME_NOW: i64 = (1 << 30) - 1;
/// Leave this timestamp alone. As above.
#[expect(dead_code)]
pub(crate) const KORU_UTIME_OMIT: i64 = (1 << 30) - 2;

/// `UTIMES`' argument, two `(seconds, nanoseconds)` pairs. 32 bytes, no
/// padding. Mirrors `struct timespec64` field for field, which is what the
/// sentinels above are expressed in; the kernel translates pair by pair.
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub(crate) struct KoruTimes {
    /// Seconds since the epoch; signed, because a date before 1970 is a date.
    pub(crate) atime_sec: i64,
    /// Nanoseconds, or one of the `KORU_UTIME_*` sentinels.
    pub(crate) atime_nsec: i64,
    pub(crate) mtime_sec: i64,
    pub(crate) mtime_nsec: i64,
}

// Size alone would not catch two fields being swapped, so assert every offset.
kernel::static_assert!(core::mem::size_of::<Sqe>() == 32);
kernel::static_assert!(core::mem::align_of::<Sqe>() == 8);
kernel::static_assert!(core::mem::offset_of!(Sqe, opcode) == 0);
kernel::static_assert!(core::mem::offset_of!(Sqe, flags) == 1);
kernel::static_assert!(core::mem::offset_of!(Sqe, rsvd0) == 2);
kernel::static_assert!(core::mem::offset_of!(Sqe, len) == 4);
kernel::static_assert!(core::mem::offset_of!(Sqe, off) == 8);
kernel::static_assert!(core::mem::offset_of!(Sqe, user_data) == 16);
kernel::static_assert!(core::mem::offset_of!(Sqe, slot) == 24);
kernel::static_assert!(core::mem::offset_of!(Sqe, handle) == 28);

kernel::static_assert!(core::mem::size_of::<Cqe>() == 32);
kernel::static_assert!(core::mem::align_of::<Cqe>() == 8);
kernel::static_assert!(core::mem::offset_of!(Cqe, user_data) == 0);
kernel::static_assert!(core::mem::offset_of!(Cqe, res) == 8);
kernel::static_assert!(core::mem::offset_of!(Cqe, flags) == 16);
kernel::static_assert!(core::mem::offset_of!(Cqe, rsvd0) == 20);
kernel::static_assert!(core::mem::offset_of!(Cqe, extra) == 24);

kernel::static_assert!(core::mem::size_of::<KoruEnter>() == 64);
kernel::static_assert!(core::mem::align_of::<KoruEnter>() == 8);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, sq_addr) == 0);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, cq_addr) == 8);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, timeout_ns) == 16);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, to_submit) == 24);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, cq_space) == 28);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, min_complete) == 32);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, flags) == 36);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, completed) == 40);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, submitted) == 44);
kernel::static_assert!(core::mem::offset_of!(KoruEnter, reserved) == 48);

kernel::static_assert!(core::mem::size_of::<KoruStat>() == 256);
kernel::static_assert!(core::mem::align_of::<KoruStat>() == 8);
kernel::static_assert!(core::mem::offset_of!(KoruStat, ino) == 0);
kernel::static_assert!(core::mem::offset_of!(KoruStat, size) == 8);
kernel::static_assert!(core::mem::offset_of!(KoruStat, blocks) == 16);
kernel::static_assert!(core::mem::offset_of!(KoruStat, blksize) == 24);
kernel::static_assert!(core::mem::offset_of!(KoruStat, nlink) == 32);
kernel::static_assert!(core::mem::offset_of!(KoruStat, mode) == 40);
kernel::static_assert!(core::mem::offset_of!(KoruStat, uid) == 48);
kernel::static_assert!(core::mem::offset_of!(KoruStat, gid) == 56);
kernel::static_assert!(core::mem::offset_of!(KoruStat, dev_major) == 64);
kernel::static_assert!(core::mem::offset_of!(KoruStat, dev_minor) == 72);
kernel::static_assert!(core::mem::offset_of!(KoruStat, rdev_major) == 80);
kernel::static_assert!(core::mem::offset_of!(KoruStat, rdev_minor) == 88);
kernel::static_assert!(core::mem::offset_of!(KoruStat, atime_sec) == 96);
kernel::static_assert!(core::mem::offset_of!(KoruStat, atime_nsec) == 104);
kernel::static_assert!(core::mem::offset_of!(KoruStat, mtime_sec) == 112);
kernel::static_assert!(core::mem::offset_of!(KoruStat, mtime_nsec) == 120);
kernel::static_assert!(core::mem::offset_of!(KoruStat, ctime_sec) == 128);
kernel::static_assert!(core::mem::offset_of!(KoruStat, ctime_nsec) == 136);
kernel::static_assert!(core::mem::offset_of!(KoruStat, btime_sec) == 144);
kernel::static_assert!(core::mem::offset_of!(KoruStat, btime_nsec) == 152);
kernel::static_assert!(core::mem::offset_of!(KoruStat, reserved) == 160);

kernel::static_assert!(core::mem::size_of::<KoruTimes>() == 32);
kernel::static_assert!(core::mem::align_of::<KoruTimes>() == 8);
kernel::static_assert!(core::mem::offset_of!(KoruTimes, atime_sec) == 0);
kernel::static_assert!(core::mem::offset_of!(KoruTimes, atime_nsec) == 8);
kernel::static_assert!(core::mem::offset_of!(KoruTimes, mtime_sec) == 16);
kernel::static_assert!(core::mem::offset_of!(KoruTimes, mtime_nsec) == 24);

// SAFETY: `repr(C)`, integers only, so every bit pattern is valid. No interior
// mutability.
unsafe impl FromBytes for Sqe {}
// SAFETY: as above. This is what lets `UTIMES` decode its argument.
unsafe impl FromBytes for KoruTimes {}
// SAFETY: as above.
unsafe impl FromBytes for Cqe {}
// SAFETY: as above.
unsafe impl FromBytes for KoruEnter {}

// SAFETY: `repr(C)` and gap-free per the offset assertions above, so no
// uninitialised bytes. No interior mutability.
unsafe impl AsBytes for Cqe {}
// SAFETY: as above.
unsafe impl AsBytes for KoruEnter {}
// SAFETY: as above. This is what lets `STAT` copy the struct out as bytes.
unsafe impl AsBytes for KoruStat {}
