// SPDX-License-Identifier: GPL-2.0

//! The koru wire format. **Canonical**; `user/cpp/include/koru_abi.h` mirrors
//! it and T16 diffs the two.
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
    /// in: must be zero.
    pub(crate) reserved: [u64; 3],
}

// 104 = eight u32 at 0..32, then u64-aligned fields; a multiple of 8, so no
// trailing padding.
kernel::static_assert!(core::mem::size_of::<KoruParams>() == 104);
kernel::static_assert!(core::mem::align_of::<KoruParams>() == 8);
// The two fields carved out of the old `reserved[3]`, and where that leaves it.
kernel::static_assert!(core::mem::offset_of!(KoruParams, handle_count) == 72);
kernel::static_assert!(core::mem::offset_of!(KoruParams, max_handles) == 76);
kernel::static_assert!(core::mem::offset_of!(KoruParams, reserved) == 80);

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
/// it. Regular files only.
pub(crate) const KORU_OP_READ: u8 = 3; // T10
/// Retire the handle in `handle`. `len`, `off` and `slot` must be zero.
pub(crate) const KORU_OP_CLOSE: u8 = 4; // T9
#[expect(dead_code)]
pub(crate) const KORU_OP_CANCEL: u8 = 5; // T11
/// FNV-1a over `len` bytes at `off` in slot `slot`, returned in `res`.
/// `handle` must be zero. Scaffolding for the arena; see doc/Notes.md.
pub(crate) const KORU_OP_CHECKSUM: u8 = 6; // T7

/// Any bit set is rejected.
pub(crate) const KORU_SQE_FLAGS_ALL: u8 = 0;

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

/// Any bit outside this completes with `EINVAL`. No `O_CREAT`: there is no
/// field for a creation mode.
pub(crate) const KORU_OPEN_FLAGS_ALL: u32 =
    KORU_O_ACCMODE | KORU_O_NOFOLLOW | KORU_O_DIRECTORY;

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
    /// Opcode-specific offset, or the delay in nanoseconds for `DELAY_NS`.
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
    /// Opcode-specific extra result. Currently always zero.
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

// SAFETY: `repr(C)`, integers only, so every bit pattern is valid. No interior
// mutability.
unsafe impl FromBytes for Sqe {}
// SAFETY: as above.
unsafe impl FromBytes for Cqe {}
// SAFETY: as above.
unsafe impl FromBytes for KoruEnter {}

// SAFETY: `repr(C)` and gap-free per the offset assertions above, so no
// uninitialised bytes. No interior mutability.
unsafe impl AsBytes for Cqe {}
// SAFETY: as above.
unsafe impl AsBytes for KoruEnter {}
