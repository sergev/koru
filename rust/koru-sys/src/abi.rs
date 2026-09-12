// SPDX-License-Identifier: GPL-2.0

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
pub const KORU_OP_OPEN: u8 = 2;
/// Read `len` bytes from file offset `off` of `handle` into slot `slot` at slot
/// offset 0. `res` is the count read: short at EOF, 0 past it. Regular files.
pub const KORU_OP_READ: u8 = 3;
/// Retire `handle`. `len`, `off`, `slot` must be zero.
pub const KORU_OP_CLOSE: u8 = 4;
/// Cancel the in-flight op whose `user_data` is `off`. `len`, `slot`, `handle`
/// must be zero. `res` 0 cancelled, `-ENOENT` not found, `-EALREADY` running.
/// The target always gets its own CQE; the order of the two is unspecified.
pub const KORU_OP_CANCEL: u8 = 5;
/// FNV-1a over `len` bytes at `off` in slot `slot`, into `res`. `handle` zero.
pub const KORU_OP_CHECKSUM: u8 = 6;

/// Any bit set is rejected.
pub const KORU_SQE_FLAGS_ALL: u8 = 0;

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

/// Any bit outside this completes `EINVAL`. No `O_CREAT`: no field carries a
/// creation mode.
pub const KORU_OPEN_FLAGS_ALL: u32 = KORU_O_ACCMODE | KORU_O_NOFOLLOW | KORU_O_DIRECTORY;

/// Multishot bit, reserved and never set.
pub const KORU_CQE_F_MORE: u32 = 1 << 0;

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
    /// Opcode-specific offset; nanoseconds for `DELAY_NS`, target `user_data`
    /// for `CANCEL`, file offset for `READ`, within-slot offset otherwise.
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
    /// Opcode-specific extra result. Currently always zero.
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
        assert_eq!(KORU_OPEN_FLAGS_ALL, 0xf);
        assert_eq!(KORU_O_ACCMODE, 0x3);
        assert_eq!(KORU_O_NOFOLLOW, 0x4);
        assert_eq!(KORU_O_DIRECTORY, 0x8);
    }

    #[test]
    fn magic_spells_koru_little_endian() {
        assert_eq!(KORU_MAGIC.to_le_bytes(), *b"koru");
    }
}
