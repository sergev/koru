// SPDX-License-Identifier: MIT

//! The koru wire format. Mirrors `kernel/koru_abi.rs`, which is canonical; T14
//! diffs the two. Pure layout: the `Sqe` constructors live in `ring.rs`.

/// Set by userspace in [`KoruParams::magic`]. "koru" little-endian.
pub const KORU_MAGIC: u32 = 0x7572_6f6b;

/// Bumped on any incompatible change to this file.
pub const KORU_ABI_VERSION: u32 = 1;

/// ioctl type byte.
pub const KORU_IOC_TYPE: u32 = 'k' as u32;

/// Command numbers. Kernel dispatch is on these, which keeps `ENOTTY` (no such
/// command) distinct from `EPROTO` (wrong size or direction).
pub const KORU_NR_SETUP: u32 = 0x00;
pub const KORU_NR_GET_PARAMS: u32 = 0x01;
pub const KORU_NR_ENTER: u32 = 0x02;

/// No flags defined yet, so any bit set is rejected.
pub const KORU_SETUP_FLAGS_ALL: u32 = 0;
pub const KORU_ENTER_FLAGS_ALL: u32 = 0;

/// Caps, reported in every [`KoruParams`]. A request above one is rejected,
/// not clamped.
pub const KORU_MAX_SQ_ENTRIES: u32 = 4096;
pub const KORU_MAX_CQ_ENTRIES: u32 = 8192;
pub const KORU_MAX_SLOT_SIZE: u32 = 1 << 20;
pub const KORU_MAX_SLOT_COUNT: u32 = 4096;
pub const KORU_MAX_ARENA_BYTES: u64 = 64 << 20;
/// Bounded by the handle encoding: the index is 16 bits.
pub const KORU_MAX_HANDLES: u32 = 4096;

/// What `handle_count == 0` means at `SETUP`.
pub const KORU_DEFAULT_HANDLES: u32 = 64;

/// Cap on a `DELAY_NS` delay: one hour.
pub const KORU_MAX_DELAY_NS: u64 = 3_600_000_000_000;

/// Ring configuration, for `SETUP` and `GET_PARAMS`.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KoruParams {
    /// in: must be [`KORU_MAGIC`], else `EPROTO`.
    pub magic: u32,
    /// in: must be [`KORU_ABI_VERSION`], else `EPROTO`.
    pub abi_version: u32,
    /// in: any bit outside [`KORU_SETUP_FLAGS_ALL`] is `EINVAL`.
    pub flags: u32,
    /// in/out: submission queue depth. Must be non-zero.
    pub sq_entries: u32,
    /// in/out: completion queue depth; 0 means `sq_entries`. Must end up
    /// `>= sq_entries`.
    pub cq_entries: u32,
    /// in/out: size of one arena slot, in bytes. Multiple of PAGE_SIZE.
    pub slot_size: u32,
    /// in/out: number of arena slots.
    pub slot_count: u32,
    /// out: non-zero once `SETUP` has succeeded on this fd.
    pub configured: u32,

    /// out: capability bits. None defined yet.
    pub features: u64,
    /// out: exact length to pass to `mmap`, `slot_size * slot_count`.
    pub arena_size: u64,
    /// out: caps. See the `KORU_MAX_*` constants.
    pub max_sq_entries: u32,
    pub max_cq_entries: u32,
    pub max_slot_size: u32,
    pub max_slot_count: u32,
    /// out: cap on `arena_size`.
    pub max_arena_bytes: u64,
    /// in/out: handle table size; 0 means [`KORU_DEFAULT_HANDLES`].
    pub handle_count: u32,
    /// out: cap on `handle_count`.
    pub max_handles: u32,
    /// out: cap on a `DELAY_NS` delay, in nanoseconds.
    pub max_delay_ns: u64,
    /// in: must be zero.
    pub reserved: [u64; 2],
}

const _: () = assert!(size_of::<KoruParams>() == 104);
const _: () = assert!(align_of::<KoruParams>() == 8);
const _: () = assert!(core::mem::offset_of!(KoruParams, handle_count) == 72);
const _: () = assert!(core::mem::offset_of!(KoruParams, max_handles) == 76);
const _: () = assert!(core::mem::offset_of!(KoruParams, max_delay_ns) == 80);
const _: () = assert!(core::mem::offset_of!(KoruParams, reserved) == 88);

// ---------------------------------------------------------------------------
// Submission and completion
// ---------------------------------------------------------------------------

/// Opcodes. ABI numbers; unimplemented ones complete `EINVAL`.
pub const KORU_OP_NOP: u8 = 0;
/// Delay `off` nanoseconds. `len`, `slot`, `handle` must be zero.
pub const KORU_OP_DELAY_NS: u8 = 1;
/// Open the path in `len` bytes at `off` in slot `slot`; `handle` carries the
/// open flags. `res` is the new handle, always positive.
///
/// With [`KORU_O_CREAT`] the slot also carries a `u64` creation mode after the
/// path, as a path op carries its argument; without it nothing past the path is
/// read.
pub const KORU_OP_OPEN: u8 = 2;
/// Read `len` bytes from file offset `off` of `handle` into slot `slot` at slot
/// offset 0. `res` is the count read: short at EOF, 0 past it. Regular files,
/// or anything opened `KORU_O_NONBLOCK`.
pub const KORU_OP_READ: u8 = 3;
/// Retire `handle`. `len`, `off`, `slot` must be zero.
pub const KORU_OP_CLOSE: u8 = 4;
/// Cancel the in-flight op whose `user_data` is `off`. `len`, `slot`, `handle`
/// must be zero. `res` 0 cancelled, `-ENOENT` not found, `-EALREADY` running.
/// The target always gets its own CQE; the order of the two is unspecified.
pub const KORU_OP_CANCEL: u8 = 5;
/// FNV-1a over `len` bytes at `off` in slot `slot`, into `res`. `handle` zero.
pub const KORU_OP_CHECKSUM: u8 = 6;
/// Write `len` bytes from slot `slot`, at slot offset 0, to file offset `off`
/// of `handle`. `res` is the count written; short is a result, not an error.
/// Regular files, or anything opened `KORU_O_NONBLOCK`. A non-zero `off` needs
/// a seekable file, else `EINVAL`; on an `O_APPEND` handle the kernel appends
/// and `off` is ignored.
pub const KORU_OP_WRITE: u8 = 7;
/// Adopt the already-open descriptor in `off` and return a handle for it.
/// `len`, `slot` and `handle` must be zero, and `off` at most `i32::MAX`.
///
/// **Grants no authority the submitting task does not already hold.** Adopting
/// any koru descriptor is `ELOOP`.
pub const KORU_OP_ADOPT_FD: u8 = 8;
/// Wait once for one of the events in `len` on `handle`. `off` and `slot` must
/// be zero; `res` is the mask that fired, always non-negative.
///
/// Single-shot: admission control leaves a multishot poll nowhere to put its
/// second completion. A file on no waitqueue — every regular file — completes
/// at once, with `res` 0 where none of the asked-for events are in its default
/// mask.
pub const KORU_OP_POLL_ADD: u8 = 9;
/// Stat `handle` into slot `slot` at slot offset `off`, which must be a
/// multiple of 8. `len` is the caller's buffer size and doubles as version
/// negotiation: the kernel writes `min(len, size_of::<KoruStat>())` bytes and
/// returns that count in `res`. `extra` is the [`KORU_STAT_INO`]-style mask of
/// fields the filesystem actually reported.
pub const KORU_OP_STAT: u8 = 10;

// Path operations. Every one names its path the way `OPEN` does: `len` bytes at
// `off` in slot `slot`, with `handle` zero. An argument that does not fit in the
// SQE follows the path in the same slot, at the first 8-aligned offset at or
// after its end.

/// Set the length of the file the path names. The argument is a `u64`, the new
/// length; `res` is 0. Follows a final symlink, as `truncate(2)` does.
pub const KORU_OP_TRUNCATE: u8 = 11;
/// Set the access and modification times of the file the path names. The
/// argument is a [`KoruTimes`]; `res` is 0. Follows a final symlink.
pub const KORU_OP_UTIMES: u8 = 12;
/// Read the symlink the path names. No argument: the target **replaces the path
/// it was given**, NUL-terminated, at `off` in the same slot, and `res` is its
/// length without the NUL. Does not follow a final symlink. `ENAMETOOLONG` if it
/// does not fit in the rest of the slot.
pub const KORU_OP_READLINK: u8 = 13;
/// Stat the file the path names. No argument: the [`KoruStat`] **replaces the
/// path it was given**, at `off` in the same slot, and `res` is its size. `off`
/// must be a multiple of eight and the whole struct must fit in the rest of the
/// slot, else `EINVAL`. `extra` carries the [`KORU_STAT_INO`]-style mask, as on
/// `STAT`. Follows a final symlink, as `stat(2)` does.
///
/// `len` is the path's length here, so there is no version negotiation as on
/// `STAT`: a later field comes out of `KoruStat::reserved`, which fixes the
/// struct's size for every binary at this ABI version.
pub const KORU_OP_STATX_AT: u8 = 14;
/// Create the directory the path names. No argument: `handle` carries the mode,
/// free on this opcode exactly as `OPEN`'s flags are, and `res` is 0. The VFS
/// applies the umask, as `mkdir(2)` does.
pub const KORU_OP_MKDIR: u8 = 15;
/// Create a symlink. **Two** NUL-terminated paths back to back in the slot, with
/// `len` covering both and their separator: the target first, then the link to
/// create, in `symlink(2)`'s own argument order. Exactly one NUL may fall inside
/// those `len` bytes and neither half may be empty. `handle` must be zero and
/// `res` is 0.
pub const KORU_OP_SYMLINK: u8 = 16;

/// Remove the file the path names. `handle` must be zero and `res` is 0.
/// Refuses a directory with `EISDIR`, as `unlink(2)` does.
pub const KORU_OP_UNLINK: u8 = 17;
/// Remove the directory the path names. `handle` must be zero and `res` is 0.
/// `ENOTDIR` for anything else, `ENOTEMPTY` for a directory with entries.
pub const KORU_OP_RMDIR: u8 = 18;

/// Rename. **Two** NUL-terminated paths back to back in the slot, as on
/// `SYMLINK`: the old path first, then the new one, in `rename(2)`'s own
/// argument order. `handle` must be zero and `res` is 0. Both halves must be on
/// one mount, else `EXDEV`; an existing destination is replaced, as `rename(2)`
/// replaces it.
pub const KORU_OP_RENAME: u8 = 19;

/// Read directory entries into slot `slot` at slot offset 0, as `READ` does.
/// `len` is the byte budget; `off` is a resume cookie, 0 for the beginning.
/// `res` is the bytes written and 0 is end of directory, mirroring `READ`;
/// `extra` is the next cookie. A budget too small for one entry is `EINVAL`,
/// not 0, or a short buffer would read as the end. `ENOTDIR` for a
/// non-directory.
///
/// **The first opcode that mutates shared per-file state**, `f_pos`, so it is
/// serialised per handle: a second concurrent one gets `EBUSY`.
pub const KORU_OP_READDIR: u8 = 20;

// A `UNLINK`, `RMDIR` or `RENAME` whose last component is empty, `.`, `..` or
// followed by a separator completes `EINVAL`, where the syscalls spread `EISDIR`,
// `ENOTEMPTY`, `EINVAL` and `EBUSY` over those same four cases. Naming the thing
// above you is a caller bug here, not an outcome.

/// What a `MKDIR` SQE's `handle` may carry: the permission bits and the sticky
/// bit, which is all `vfs_mkdir` keeps of a requested mode. Any other bit is
/// `EINVAL` rather than silently dropped, as an unknown open flag is.
pub const KORU_MKDIR_MODE_ALL: u32 = 0o1777;

/// Any bit set is rejected.
pub const KORU_SQE_FLAGS_ALL: u8 = 0;

// Poll events, in a `POLL_ADD` SQE's `len` and returned in `res`. koru's own
// bit values, like the open flags.

/// Readable, or end of file on a stream.
pub const KORU_POLL_IN: u32 = 1 << 0;
/// Writable.
pub const KORU_POLL_OUT: u32 = 1 << 1;
/// Out-of-band data.
pub const KORU_POLL_PRI: u32 = 1 << 2;
/// The peer closed its writing half.
pub const KORU_POLL_RDHUP: u32 = 1 << 3;
/// Error. Reported whether or not it was asked for.
pub const KORU_POLL_ERR: u32 = 1 << 4;
/// Hang-up. Reported whether or not it was asked for.
pub const KORU_POLL_HUP: u32 = 1 << 5;

/// Any bit outside this completes `EINVAL`, and so does an empty mask.
pub const KORU_POLL_EVENTS_ALL: u32 =
    KORU_POLL_IN | KORU_POLL_OUT | KORU_POLL_PRI | KORU_POLL_RDHUP | KORU_POLL_ERR | KORU_POLL_HUP;

// Open flags, in an `OPEN` SQE's `handle`. koru's own bit values, not host
// `O_*`; the kernel translates.

/// Access mode: the low two bits. Mode 3 is invalid.
pub const KORU_O_ACCMODE: u32 = 0x3;
pub const KORU_O_RDONLY: u32 = 0;
pub const KORU_O_WRONLY: u32 = 1;
pub const KORU_O_RDWR: u32 = 2;

/// `ELOOP` rather than following a final symlink.
pub const KORU_O_NOFOLLOW: u32 = 1 << 2;
/// `ENOTDIR` unless the path names a directory.
pub const KORU_O_DIRECTORY: u32 = 1 << 3;
/// Open without blocking, and make `READ` and `WRITE` answer `EAGAIN` rather
/// than wait. Required to `READ` or `WRITE` anything but a regular file.
///
/// **koru never sets or clears `O_NONBLOCK` on a file it did not open.**
pub const KORU_O_NONBLOCK: u32 = 1 << 4;

/// Create the file if it is not there. The creation mode is a `u64` argument
/// **after the path in the same slot**, at [`crate::ring::arg_offset`], where
/// every path op puts its argument. Read only when this bit is set.
pub const KORU_O_CREAT: u32 = 1 << 5;
/// With [`KORU_O_CREAT`], `EEXIST` if the path is already there. Without it,
/// `EINVAL`: a flag that cannot act is rejected, not dropped.
pub const KORU_O_EXCL: u32 = 1 << 6;
/// Truncate an existing regular file to zero length on the way in.
pub const KORU_O_TRUNC: u32 = 1 << 7;
/// Every `WRITE` on this handle appends and its `off` is ignored.
pub const KORU_O_APPEND: u32 = 1 << 8;

/// Any bit outside this completes `EINVAL`.
pub const KORU_OPEN_FLAGS_ALL: u32 = KORU_O_ACCMODE
    | KORU_O_NOFOLLOW
    | KORU_O_DIRECTORY
    | KORU_O_NONBLOCK
    | KORU_O_CREAT
    | KORU_O_EXCL
    | KORU_O_TRUNC
    | KORU_O_APPEND;

/// What a [`KORU_O_CREAT`] open's mode argument may carry, which is
/// [`KORU_MKDIR_MODE_ALL`]'s rule for the same reason: `S_ISUID` and `S_ISGID`
/// are refused rather than silently dropped, and koru creates nothing setuid.
pub const KORU_OPEN_MODE_ALL: u64 = 0o1777;

/// Multishot bit, reserved and never set.
pub const KORU_CQE_F_MORE: u32 = 1 << 0;

/// `READDIR` dropped an entry whose name it could not represent: a NUL or a
/// separator in it, which means a corrupt filesystem.
pub const KORU_CQE_F_SKIPPED: u32 = 1 << 1;

/// A submission queue entry. Fields an opcode does not read must be zero.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct Sqe {
    /// One of the `KORU_OP_*` constants.
    pub opcode: u8,
    /// Any bit outside [`KORU_SQE_FLAGS_ALL`] completes `EINVAL`.
    pub flags: u8,
    /// Must be zero.
    pub rsvd0: u16,
    /// Opcode-specific length.
    pub len: u32,
    /// Opcode-specific offset. A file offset on `READ` and `WRITE`, a
    /// within-slot offset on `OPEN`, `CHECKSUM` and `STAT`, nanoseconds on
    /// `DELAY_NS`, the target's `user_data` on `CANCEL`, a file descriptor on
    /// `ADOPT_FD`. `NOP` and `CLOSE` want zero.
    pub off: u64,
    /// Echoed into the CQE. Opaque.
    pub user_data: u64,
    /// Arena slot index. Buffers are named by index, never by address.
    pub slot: u32,
    /// Handle: index low, generation high. Never 0 when valid. On `OPEN` this
    /// carries the `KORU_O_*` flags instead.
    pub handle: u32,
}

/// A completion queue entry. 32 bytes, so one never straddles a cache line.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct Cqe {
    /// Copied from the SQE that produced this completion.
    pub user_data: u64,
    /// `>= 0` on success, negative errno on failure.
    pub res: i64,
    /// Currently always zero. See [`KORU_CQE_F_MORE`].
    pub flags: u32,
    /// Must be zero.
    pub rsvd0: u32,
    /// Opcode-specific extra result, zero on every opcode that defines none.
    /// `STAT` puts the mask of fields it filled here.
    pub extra: u64,
}

/// The `ENTER` ioctl argument.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KoruEnter {
    /// in: address of an array of at least `to_submit` [`Sqe`].
    pub sq_addr: u64,
    /// in: address of an array of at least `cq_space` [`Cqe`].
    pub cq_addr: u64,
    /// in: relative MONOTONIC cap on the wait; 0 means no cap. Only consulted
    /// when `min_complete > 0`. Rounds to jiffy granularity.
    pub timeout_ns: u64,
    /// in: number of SQEs to consume.
    pub to_submit: u32,
    /// in: capacity of the CQE array, in entries, not bytes.
    pub cq_space: u32,
    /// in: completions to wait for. 0 returns immediately.
    pub min_complete: u32,
    /// in: any bit outside [`KORU_ENTER_FLAGS_ALL`] fails the ioctl.
    pub flags: u32,
    /// out: CQEs written. Separate from the ioctl return, which E1 reserves for
    /// SQEs consumed.
    pub completed: u32,
    /// out: SQEs consumed. Written on every path, `EINTR` included.
    pub submitted: u32,
    /// in: must be zero.
    pub reserved: [u64; 2],
}

// Size alone would not catch two fields being swapped, so assert every offset.
const _: () = assert!(size_of::<Sqe>() == 32);
const _: () = assert!(align_of::<Sqe>() == 8);
const _: () = assert!(core::mem::offset_of!(Sqe, opcode) == 0);
const _: () = assert!(core::mem::offset_of!(Sqe, flags) == 1);
const _: () = assert!(core::mem::offset_of!(Sqe, rsvd0) == 2);
const _: () = assert!(core::mem::offset_of!(Sqe, len) == 4);
const _: () = assert!(core::mem::offset_of!(Sqe, off) == 8);
const _: () = assert!(core::mem::offset_of!(Sqe, user_data) == 16);
const _: () = assert!(core::mem::offset_of!(Sqe, slot) == 24);
const _: () = assert!(core::mem::offset_of!(Sqe, handle) == 28);

const _: () = assert!(size_of::<Cqe>() == 32);
const _: () = assert!(align_of::<Cqe>() == 8);
const _: () = assert!(core::mem::offset_of!(Cqe, user_data) == 0);
const _: () = assert!(core::mem::offset_of!(Cqe, res) == 8);
const _: () = assert!(core::mem::offset_of!(Cqe, flags) == 16);
const _: () = assert!(core::mem::offset_of!(Cqe, rsvd0) == 20);
const _: () = assert!(core::mem::offset_of!(Cqe, extra) == 24);

const _: () = assert!(size_of::<KoruEnter>() == 64);
const _: () = assert!(align_of::<KoruEnter>() == 8);
const _: () = assert!(core::mem::offset_of!(KoruEnter, sq_addr) == 0);
const _: () = assert!(core::mem::offset_of!(KoruEnter, cq_addr) == 8);
const _: () = assert!(core::mem::offset_of!(KoruEnter, timeout_ns) == 16);
const _: () = assert!(core::mem::offset_of!(KoruEnter, to_submit) == 24);
const _: () = assert!(core::mem::offset_of!(KoruEnter, cq_space) == 28);
const _: () = assert!(core::mem::offset_of!(KoruEnter, min_complete) == 32);
const _: () = assert!(core::mem::offset_of!(KoruEnter, flags) == 36);
// completed precedes submitted.
const _: () = assert!(core::mem::offset_of!(KoruEnter, completed) == 40);
const _: () = assert!(core::mem::offset_of!(KoruEnter, submitted) == 44);
const _: () = assert!(core::mem::offset_of!(KoruEnter, reserved) == 48);

// ---------------------------------------------------------------------------
// Stat
// ---------------------------------------------------------------------------

// File type, the top bits of [`KoruStat::mode`]. Unlike the open flags, these
// values are the same on every Linux architecture, so they are passed through.

pub const KORU_S_IFMT: u64 = 0o170000;
pub const KORU_S_IFIFO: u64 = 0o010000;
pub const KORU_S_IFCHR: u64 = 0o020000;
pub const KORU_S_IFDIR: u64 = 0o040000;
pub const KORU_S_IFBLK: u64 = 0o060000;
pub const KORU_S_IFREG: u64 = 0o100000;
pub const KORU_S_IFLNK: u64 = 0o120000;
pub const KORU_S_IFSOCK: u64 = 0o140000;

// Which [`KoruStat`] fields the kernel filled, returned in `Cqe::extra`.
// koru's own bits, one per field and in this struct's field order, not statx's.
// `blksize`, `dev` and `rdev` have no statx bit because the VFS always fills
// them, and they get one here so the mask describes the whole struct.

pub const KORU_STAT_INO: u64 = 1 << 0;
pub const KORU_STAT_SIZE: u64 = 1 << 1;
pub const KORU_STAT_BLOCKS: u64 = 1 << 2;
pub const KORU_STAT_BLKSIZE: u64 = 1 << 3;
pub const KORU_STAT_NLINK: u64 = 1 << 4;
pub const KORU_STAT_TYPE: u64 = 1 << 5;
pub const KORU_STAT_MODE: u64 = 1 << 6;
pub const KORU_STAT_UID: u64 = 1 << 7;
pub const KORU_STAT_GID: u64 = 1 << 8;
pub const KORU_STAT_DEV: u64 = 1 << 9;
pub const KORU_STAT_RDEV: u64 = 1 << 10;
pub const KORU_STAT_ATIME: u64 = 1 << 11;
pub const KORU_STAT_MTIME: u64 = 1 << 12;
pub const KORU_STAT_CTIME: u64 = 1 << 13;
pub const KORU_STAT_BTIME: u64 = 1 << 14;

/// Everything `STAT` can report. A filesystem may report less; never more.
pub const KORU_STAT_ALL: u64 = KORU_STAT_INO
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
/// padding. Times are second-plus-nanosecond pairs and device numbers are
/// explicit major and minor, so nothing here is a kernel-internal encoding.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KoruStat {
    pub ino: u64,
    pub size: u64,
    /// 512-byte blocks allocated.
    pub blocks: u64,
    pub blksize: u64,
    pub nlink: u64,
    /// File type in [`KORU_S_IFMT`], permission bits below it.
    pub mode: u64,
    /// Translated into the submitting task's user namespace.
    pub uid: u64,
    pub gid: u64,
    pub dev_major: u64,
    pub dev_minor: u64,
    pub rdev_major: u64,
    pub rdev_minor: u64,
    /// Seconds since the epoch; signed, because a date before 1970 is a date.
    pub atime_sec: i64,
    pub atime_nsec: u64,
    pub mtime_sec: i64,
    pub mtime_nsec: u64,
    pub ctime_sec: i64,
    pub ctime_nsec: u64,
    /// Creation time. Absent from most filesystems; [`KORU_STAT_BTIME`] says.
    pub btime_sec: i64,
    pub btime_nsec: u64,
    /// out: must read as zero.
    pub reserved: [u64; 12],
}

const _: () = assert!(size_of::<KoruStat>() == 256);
const _: () = assert!(align_of::<KoruStat>() == 8);
const _: () = assert!(core::mem::offset_of!(KoruStat, ino) == 0);
const _: () = assert!(core::mem::offset_of!(KoruStat, size) == 8);
const _: () = assert!(core::mem::offset_of!(KoruStat, blocks) == 16);
const _: () = assert!(core::mem::offset_of!(KoruStat, blksize) == 24);
const _: () = assert!(core::mem::offset_of!(KoruStat, nlink) == 32);
const _: () = assert!(core::mem::offset_of!(KoruStat, mode) == 40);
const _: () = assert!(core::mem::offset_of!(KoruStat, uid) == 48);
const _: () = assert!(core::mem::offset_of!(KoruStat, gid) == 56);
const _: () = assert!(core::mem::offset_of!(KoruStat, dev_major) == 64);
const _: () = assert!(core::mem::offset_of!(KoruStat, dev_minor) == 72);
const _: () = assert!(core::mem::offset_of!(KoruStat, rdev_major) == 80);
const _: () = assert!(core::mem::offset_of!(KoruStat, rdev_minor) == 88);
const _: () = assert!(core::mem::offset_of!(KoruStat, atime_sec) == 96);
const _: () = assert!(core::mem::offset_of!(KoruStat, atime_nsec) == 104);
const _: () = assert!(core::mem::offset_of!(KoruStat, mtime_sec) == 112);
const _: () = assert!(core::mem::offset_of!(KoruStat, mtime_nsec) == 120);
const _: () = assert!(core::mem::offset_of!(KoruStat, ctime_sec) == 128);
const _: () = assert!(core::mem::offset_of!(KoruStat, ctime_nsec) == 136);
const _: () = assert!(core::mem::offset_of!(KoruStat, btime_sec) == 144);
const _: () = assert!(core::mem::offset_of!(KoruStat, btime_nsec) == 152);
const _: () = assert!(core::mem::offset_of!(KoruStat, reserved) == 160);

// ---------------------------------------------------------------------------
// Directory entries
// ---------------------------------------------------------------------------

// A [`KoruDirent`]'s type: `(i_mode & S_IFMT) >> 12`, the same on every
// architecture, so they pass through as the `S_IF*` values do.

pub const KORU_DT_UNKNOWN: u8 = 0;
pub const KORU_DT_FIFO: u8 = 1;
pub const KORU_DT_CHR: u8 = 2;
pub const KORU_DT_DIR: u8 = 4;
pub const KORU_DT_BLK: u8 = 6;
pub const KORU_DT_REG: u8 = 8;
pub const KORU_DT_LNK: u8 = 10;
pub const KORU_DT_SOCK: u8 = 12;
/// The mask the VFS applies; anything above it is a flag, not a type.
pub const KORU_DT_MASK: u8 = 0xf;

/// Every record starts on a multiple of this.
pub const KORU_DIRENT_ALIGN: usize = 8;

/// One entry's header, then `namelen` name bytes, a NUL, and padding to
/// [`KORU_DIRENT_ALIGN`]. 24 bytes, no padding.
///
/// `linux_dirent64` with its annoyances fixed: 8-aligned rather than 2-aligned,
/// and `namelen` explicit rather than implied by `reclen`. The name is
/// NUL-terminated for a C caller, but **`namelen` is authoritative**.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KoruDirent {
    pub ino: u64,
    /// Pass this as the next `off` to resume **after** this entry.
    pub cookie: u64,
    /// Header, name, NUL and padding: the stride to the next record.
    pub reclen: u16,
    /// The name's length without its NUL.
    pub namelen: u16,
    /// One of the `KORU_DT_*` values.
    pub dtype: u8,
    /// out: must read as zero.
    pub reserved: [u8; 3],
}

const _: () = assert!(size_of::<KoruDirent>() == 24);
const _: () = assert!(align_of::<KoruDirent>() == 8);
const _: () = assert!(core::mem::offset_of!(KoruDirent, ino) == 0);
const _: () = assert!(core::mem::offset_of!(KoruDirent, cookie) == 8);
const _: () = assert!(core::mem::offset_of!(KoruDirent, reclen) == 16);
const _: () = assert!(core::mem::offset_of!(KoruDirent, namelen) == 18);
const _: () = assert!(core::mem::offset_of!(KoruDirent, dtype) == 20);
const _: () = assert!(core::mem::offset_of!(KoruDirent, reserved) == 21);

// ---------------------------------------------------------------------------
// Times
// ---------------------------------------------------------------------------

// Nanosecond sentinels, checked by the kernel itself. Same values everywhere,
// so they pass through like the `S_IF*` ones.

/// Set this timestamp to now, ignoring the seconds field.
pub const KORU_UTIME_NOW: i64 = (1 << 30) - 1;
/// Leave this timestamp alone.
pub const KORU_UTIME_OMIT: i64 = (1 << 30) - 2;

/// `UTIMES`' argument, two `(seconds, nanoseconds)` pairs. 32 bytes, no padding.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug, PartialEq, Eq)]
pub struct KoruTimes {
    /// Seconds since the epoch; signed, because a date before 1970 is a date.
    pub atime_sec: i64,
    /// Nanoseconds, or one of the `KORU_UTIME_*` sentinels.
    pub atime_nsec: i64,
    pub mtime_sec: i64,
    pub mtime_nsec: i64,
}

const _: () = assert!(size_of::<KoruTimes>() == 32);
const _: () = assert!(align_of::<KoruTimes>() == 8);
const _: () = assert!(core::mem::offset_of!(KoruTimes, atime_sec) == 0);
const _: () = assert!(core::mem::offset_of!(KoruTimes, atime_nsec) == 8);
const _: () = assert!(core::mem::offset_of!(KoruTimes, mtime_sec) == 16);
const _: () = assert!(core::mem::offset_of!(KoruTimes, mtime_nsec) == 24);

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

/// Index half of a handle.
pub const fn handle_index(handle: u32) -> u16 {
    (handle & 0xffff) as u16
}

/// Generation half. Never 0 for a handle the kernel issued.
pub const fn handle_generation(handle: u32) -> u16 {
    (handle >> 16) as u16
}

/// Build a handle from its halves. Tests use it to forge stale ones.
pub const fn make_handle(index: u16, generation: u16) -> u32 {
    (index as u32) | ((generation as u32) << 16)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn handle_halves_round_trip() {
        let h = make_handle(7, 3);
        assert_eq!(h, 0x0003_0007);
        assert_eq!(handle_index(h), 7);
        assert_eq!(handle_generation(h), 3);
    }

    #[test]
    fn a_valid_handle_is_never_zero() {
        assert_eq!(make_handle(0, 1), 0x0001_0000);
    }

    #[test]
    fn open_flags_mask_covers_exactly_the_defined_bits() {
        assert_eq!(KORU_OPEN_FLAGS_ALL, 0x1ff);
        assert_eq!(KORU_O_ACCMODE, 0x3);
        assert_eq!(KORU_O_NOFOLLOW, 0x4);
        assert_eq!(KORU_O_DIRECTORY, 0x8);
        assert_eq!(KORU_O_NONBLOCK, 0x10);
        assert_eq!(KORU_O_CREAT, 0x20);
        assert_eq!(KORU_O_EXCL, 0x40);
        assert_eq!(KORU_O_TRUNC, 0x80);
        assert_eq!(KORU_O_APPEND, 0x100);
        // The mode is not a flag: it rides after the path, not in `handle`.
        assert_eq!(KORU_OPEN_MODE_ALL, 0o1777);
    }

    #[test]
    fn magic_spells_koru_little_endian() {
        assert_eq!(KORU_MAGIC.to_le_bytes(), *b"koru");
    }
}
