// SPDX-License-Identifier: GPL-2.0

//! The xring wire format. **This file is canonical.**
//!
//! `user/cpp/include/xring_abi.h` mirrors it byte for byte, and the T16
//! conformance test diffs an `abi_dump` emitted by each side. When you change
//! anything here, change the mirror and run that diff.
//!
//! Rules that apply to every structure below, from `Plan.md`:
//!
//! - Reserved fields must be zero on input. Unknown flag bits are rejected.
//!   This is the only thing that permits adding fields later without breaking
//!   old binaries.
//! - No padding anywhere. `AsBytes` forbids uninitialised bytes, and padding
//!   would copy uninitialised kernel memory out to userspace.

use kernel::error::Error;
use kernel::ioctl::{_IOR, _IOWR};
use kernel::transmute::{AsBytes, FromBytes};

/// `EPROTO`, returned when `magic` or `abi_version` does not match.
///
/// A binding built against a different revision of this file gets an
/// unambiguous answer, distinct from the `EINVAL` used for bad field values.
/// `kernel::error::code` does not declare `EPROTO`, so take it from the uapi
/// headers rather than hardcoding a number that varies by architecture.
pub(crate) fn eproto() -> Error {
    Error::from_errno(-(kernel::uapi::EPROTO as i32))
}

/// Set by userspace in [`XringParams::magic`]. Spells "xrng" little-endian.
pub(crate) const XRING_MAGIC: u32 = 0x676e_7278;

/// Bumped on any incompatible change to this file.
pub(crate) const XRING_ABI_VERSION: u32 = 1;

/// ioctl type byte. Unused in `Documentation/userspace-api/ioctl/ioctl-number.rst`.
pub(crate) const XRING_IOC_TYPE: u32 = 'x' as u32;

/// Command numbers, dispatched on separately from the size the caller encoded.
///
/// Dispatching on the whole ioctl number would fold "you asked for something we
/// do not implement" together with "you were built against a different ABI",
/// and those need different answers: `ENOTTY` and `EPROTO` respectively.
pub(crate) const XRING_NR_SETUP: u32 = 0x00;
pub(crate) const XRING_NR_GET_PARAMS: u32 = 0x01;

// The two constants below are the numbers userspace actually passes to
// `ioctl()`, and the C mirror must reproduce them exactly. The kernel does not
// reference them because it dispatches on the command number instead, so they
// read as dead code; they are not. T16's `abi_dump` emits them.

/// Configure the ring. Callable exactly once per fd, before `mmap`.
///
/// Bidirectional: userspace supplies its request, the kernel writes back the
/// effective values and the caps.
#[expect(dead_code)]
pub(crate) const XRING_IOC_SETUP: u32 = _IOWR::<XringParams>(XRING_IOC_TYPE, XRING_NR_SETUP);

/// Read the current parameters. Legal before `SETUP`, in which case only the
/// caps and the ABI fields are meaningful and `configured` is 0.
#[expect(dead_code)]
pub(crate) const XRING_IOC_GET_PARAMS: u32 =
    _IOR::<XringParams>(XRING_IOC_TYPE, XRING_NR_GET_PARAMS);

/// No setup flags are defined yet, so any bit set is rejected.
pub(crate) const XRING_SETUP_FLAGS_ALL: u32 = 0;

/// Kernel-side caps, reported in every [`XringParams`].
///
/// These are limits, not defaults. A request above any of them is rejected with
/// `EINVAL` rather than being clamped, so userspace reads them first and asks
/// for something legal. Reporting them here rather than as constants in the
/// mirrored header means raising a limit is not an ABI change.
///
/// The arena is unreclaimable, unswappable and charged to nothing, so
/// `MAX_ARENA_BYTES` is the load-bearing one. `Plan.md` calls this out.
pub(crate) const XRING_MAX_SQ_ENTRIES: u32 = 4096;
pub(crate) const XRING_MAX_CQ_ENTRIES: u32 = 8192;
pub(crate) const XRING_MAX_SLOT_SIZE: u32 = 1 << 20;
pub(crate) const XRING_MAX_SLOT_COUNT: u32 = 4096;
pub(crate) const XRING_MAX_ARENA_BYTES: u64 = 64 << 20;

/// Ring configuration, exchanged by [`XRING_IOC_SETUP`] and
/// [`XRING_IOC_GET_PARAMS`].
///
/// Direction of each field is marked: `in` is set by userspace and read by the
/// kernel, `out` is written by the kernel and ignored on input.
///
/// Field order is chosen so every field is naturally aligned and the struct has
/// no padding: eight `u32` first, then the `u64`s at offset 32.
#[repr(C)]
#[derive(Copy, Clone, Default)]
pub(crate) struct XringParams {
    /// in: must be [`XRING_MAGIC`], else `EPROTO`.
    pub(crate) magic: u32,
    /// in: must be [`XRING_ABI_VERSION`], else `EPROTO`.
    pub(crate) abi_version: u32,
    /// in: setup flags. Any bit outside [`XRING_SETUP_FLAGS_ALL`] is `EINVAL`.
    pub(crate) flags: u32,
    /// in/out: submission queue depth. Must be non-zero.
    pub(crate) sq_entries: u32,
    /// in/out: completion queue depth. 0 means "same as `sq_entries`".
    ///
    /// Must end up `>= sq_entries`: admission control reserves a CQ slot when
    /// an SQE is consumed, which is what makes CQ overflow unrepresentable.
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
    /// out: caps. See the `XRING_MAX_*` constants.
    pub(crate) max_sq_entries: u32,
    pub(crate) max_cq_entries: u32,
    pub(crate) max_slot_size: u32,
    pub(crate) max_slot_count: u32,
    /// out: cap on `arena_size`.
    pub(crate) max_arena_bytes: u64,
    /// in: must be zero.
    pub(crate) reserved: [u64; 4],
}

// The mirrored C header must agree with these. A change here that does not
// change the header is caught at T16, but only if these numbers are checked.
//
// 104 = eight u32 at offsets 0..32, then u64-aligned fields to 104. 104 is a
// multiple of the 8-byte alignment, so there is no trailing padding either.
kernel::static_assert!(core::mem::size_of::<XringParams>() == 104);
kernel::static_assert!(core::mem::align_of::<XringParams>() == 8);

// SAFETY: `XringParams` is `repr(C)`, contains only unsigned integers and an
// array of them, so every bit pattern is a valid value. It has no interior
// mutability.
unsafe impl FromBytes for XringParams {}

// SAFETY: `XringParams` is `repr(C)` and, by the field ordering above, has no
// padding: eight 4-byte fields fill offsets 0..32, and every field from offset
// 32 on is 8-byte sized and aligned. So it contains no uninitialised bytes. It
// has no interior mutability.
unsafe impl AsBytes for XringParams {}
