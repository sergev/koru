// SPDX-License-Identifier: MIT

//! The ring: `SETUP`, `ENTER`, and the mmap'd arena.

use crate::abi::*;
use crate::error::{EINVAL, Errno};
use crate::sys;
use std::ffi::c_void;
use std::io;
use std::mem::ManuallyDrop;
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, OwnedFd};
use std::ptr::NonNull;
use std::time::Duration;

pub const DEV_KORU: &str = "/dev/koru";

/// What to ask `SETUP` for. Zero means "kernel default" for `cq_entries` and
/// `handle_count`.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct SetupConfig {
    pub sq_entries: u32,
    pub cq_entries: u32,
    pub slot_size: u32,
    pub slot_count: u32,
    pub handle_count: u32,
    pub flags: u32,
}

impl SetupConfig {
    pub fn new(
        sq_entries: u32,
        cq_entries: u32,
        slot_size: u32,
        slot_count: u32,
        handle_count: u32,
    ) -> Self {
        SetupConfig {
            sq_entries,
            cq_entries,
            slot_size,
            slot_count,
            handle_count,
            flags: 0,
        }
    }

    pub fn to_params(&self) -> KoruParams {
        KoruParams {
            magic: KORU_MAGIC,
            abi_version: KORU_ABI_VERSION,
            flags: self.flags,
            sq_entries: self.sq_entries,
            cq_entries: self.cq_entries,
            slot_size: self.slot_size,
            slot_count: self.slot_count,
            handle_count: self.handle_count,
            ..KoruParams::default()
        }
    }
}

/// What `ENTER` wrote back. Valid on the error path too.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct Progress {
    pub submitted: u32,
    pub completed: u32,
}

/// A successful `ENTER`. `consumed` is the ioctl return, SQEs consumed, never
/// the completion count.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Entered {
    pub consumed: u32,
    pub progress: Progress,
}

impl Entered {
    /// The CQEs actually written into the caller's array.
    pub fn cqes<'c>(&self, cq: &'c [Cqe]) -> &'c [Cqe] {
        &cq[..self.progress.completed as usize]
    }
}

/// A failed `ENTER`, carrying the writeback: on `EINTR` the kernel still says
/// what it consumed and completed.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct EnterError {
    pub errno: Errno,
    pub progress: Progress,
}

impl EnterError {
    pub fn is_interrupted(&self) -> bool {
        self.errno == crate::error::EINTR
    }
}

impl From<EnterError> for io::Error {
    fn from(e: EnterError) -> io::Error {
        e.errno.as_io()
    }
}

/// Drops the writeback: keep the `EnterError` itself to resubmit.
impl From<EnterError> for crate::error::Error {
    fn from(e: EnterError) -> crate::error::Error {
        crate::error::Error::from_errno(e.errno)
    }
}

impl std::fmt::Display for EnterError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "ENTER: {:?}, {:?}", self.errno, self.progress)
    }
}

impl std::error::Error for EnterError {}

pub struct Ring {
    fd: OwnedFd,
    params: KoruParams,
    configured: bool,
}

impl Ring {
    /// Open the device. No `SETUP`.
    pub fn open() -> io::Result<Ring> {
        Ring::open_at(DEV_KORU)
    }

    pub fn open_at(path: &str) -> io::Result<Ring> {
        let f = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(path)?;
        Ok(Ring {
            fd: OwnedFd::from(f),
            params: KoruParams::default(),
            configured: false,
        })
    }

    /// Open and configure.
    pub fn with_config(cfg: &SetupConfig) -> io::Result<Ring> {
        let mut r = Ring::open()?;
        r.setup(cfg)?;
        Ok(r)
    }

    pub fn as_fd(&self) -> BorrowedFd<'_> {
        self.fd.as_fd()
    }

    pub fn as_raw_fd(&self) -> i32 {
        self.fd.as_raw_fd()
    }

    /// `SETUP`. One shot per fd; a second is `EBUSY`.
    pub fn setup(&mut self, cfg: &SetupConfig) -> io::Result<&KoruParams> {
        let mut p = cfg.to_params();
        sys::ioctl_setup(self.fd.as_fd(), &mut p)?;
        self.params = p;
        self.configured = true;
        Ok(&self.params)
    }

    /// `SETUP` with a caller-built struct, for the rejection matrix.
    pub fn setup_raw(&mut self, p: &mut KoruParams) -> io::Result<()> {
        sys::ioctl_setup(self.fd.as_fd(), p)?;
        self.params = *p;
        self.configured = true;
        Ok(())
    }

    pub fn get_params(&self) -> io::Result<KoruParams> {
        let mut p = KoruParams::default();
        sys::ioctl_get_params(self.fd.as_fd(), &mut p)?;
        Ok(p)
    }

    /// The values `SETUP` returned. Zeroed before a successful `SETUP`.
    pub fn params(&self) -> &KoruParams {
        &self.params
    }

    pub fn is_configured(&self) -> bool {
        self.configured
    }

    /// Map the arena. One shot in the kernel; not gated here, so the second
    /// call's `EBUSY` stays testable.
    ///
    /// The result does not borrow the `Ring`: the mapping outlives `close(fd)`.
    pub fn mmap(&self) -> io::Result<Arena> {
        Arena::map(
            self.fd.as_fd(),
            self.params.arena_size as usize,
            self.params.slot_size,
            self.params.slot_count,
        )
    }

    /// `ENTER`.
    ///
    /// A short `submitted` or `completed` is normal, not an error: admission
    /// control produces the first, a timeout or quiescence the second.
    pub fn enter(
        &self,
        sq: &[Sqe],
        cq: &mut [Cqe],
        min_complete: u32,
        timeout: Option<Duration>,
    ) -> Result<Entered, EnterError> {
        // timeout_ns 0 means "no cap", so Some(ZERO) would silently invert the
        // caller's intent. Use enter_raw to send a literal 0 with a wait.
        let timeout_ns = match timeout {
            None => 0,
            Some(d) if d.is_zero() => {
                return Err(EnterError {
                    errno: EINVAL,
                    progress: Progress::default(),
                });
            }
            Some(d) => d.as_nanos().min(u64::MAX as u128) as u64,
        };
        if min_complete as usize > cq.len() {
            return Err(EnterError {
                errno: EINVAL,
                progress: Progress::default(),
            });
        }
        let mut e = KoruEnter {
            sq_addr: sq.as_ptr() as u64,
            cq_addr: cq.as_mut_ptr() as u64,
            timeout_ns,
            to_submit: sq.len() as u32,
            cq_space: cq.len() as u32,
            min_complete,
            ..KoruEnter::default()
        };
        // SAFETY: both addresses come from live slices, and cq is borrowed
        // exclusively for the call.
        unsafe { self.enter_raw(&mut e) }
    }

    /// `ENTER` with a caller-built struct, for the rejection matrix.
    ///
    /// Reads the writeback before the return value, so `EINTR` keeps it.
    ///
    /// # Safety
    /// `e.sq_addr` and `e.cq_addr` are bare integers the kernel will read and
    /// write.
    pub unsafe fn enter_raw(&self, e: &mut KoruEnter) -> Result<Entered, EnterError> {
        // SAFETY: the caller guarantees the two addresses.
        let r = unsafe { sys::ioctl_enter(self.fd.as_fd(), e) };
        // Read the writeback before the return value, or EINTR loses it.
        let progress = Progress {
            submitted: e.submitted,
            completed: e.completed,
        };
        match r {
            Ok(consumed) => Ok(Entered { consumed, progress }),
            Err(err) => Err(EnterError {
                errno: Errno(err.raw_os_error().unwrap_or(0)),
                progress,
            }),
        }
    }

    /// Raw ioctl, for the dispatch matrix.
    ///
    /// # Safety
    /// `arg` must match what `request` says the kernel will touch.
    pub unsafe fn ioctl_raw(&self, request: u32, arg: *mut c_void) -> io::Result<i32> {
        // SAFETY: the caller guarantees the argument.
        unsafe { sys::ioctl_raw(self.fd.as_fd(), request, arg) }
    }

    /// Submit without reaping.
    pub fn submit(&self, sq: &[Sqe]) -> Result<Entered, EnterError> {
        self.enter(sq, &mut [], 0, None)
    }

    /// Submit one SQE and wait for one completion. `Ok(None)` means nothing
    /// completed, which a cancelled or deferred op can produce.
    pub fn run_one(&self, sqe: &Sqe) -> Result<Option<i64>, EnterError> {
        let mut cq = [Cqe::default(); 1];
        let r = self.enter(std::slice::from_ref(sqe), &mut cq, 1, None)?;
        Ok(if r.progress.completed == 1 {
            Some(cq[0].res)
        } else {
            None
        })
    }

    /// Drain completions until three consecutive empty rounds. Returns how many
    /// were found; a stray count means a section walked away from queued work.
    ///
    /// One empty reap is not enough: on a busy ring that is just the timeout.
    pub fn quiesce(&self) -> Result<u32, EnterError> {
        let space = self.params.cq_entries.clamp(1, 128) as usize;
        let mut cq = vec![Cqe::default(); space];
        let (mut found, mut empty) = (0u32, 0u32);
        for _ in 0..64 {
            let r = self.enter(&[], &mut cq, 1, Some(Duration::from_millis(50)))?;
            if r.progress.completed == 0 {
                empty += 1;
                if empty == 3 {
                    break;
                }
            } else {
                empty = 0;
                found += r.progress.completed;
            }
        }
        Ok(found)
    }
}

/// The mmap'd arena. Deliberately not tied to the `Ring`'s lifetime: the
/// mapping stays valid after `close(fd)`.
pub struct Arena {
    ptr: NonNull<u8>,
    len: usize,
    slot_size: u32,
    slot_count: u32,
}

impl Arena {
    fn map(fd: BorrowedFd<'_>, len: usize, slot_size: u32, slot_count: u32) -> io::Result<Arena> {
        // SAFETY: a fresh anonymous-address mapping of our own device.
        let p = unsafe {
            sys::mmap(
                std::ptr::null_mut(),
                len,
                sys::PROT_READ | sys::PROT_WRITE,
                sys::MAP_SHARED,
                fd.as_raw_fd(),
                0,
            )
        };
        if p == sys::MAP_FAILED {
            return Err(io::Error::last_os_error());
        }
        Ok(Arena {
            ptr: NonNull::new(p.cast()).unwrap(),
            len,
            slot_size,
            slot_count,
        })
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn slot_size(&self) -> u32 {
        self.slot_size
    }

    pub fn slot_count(&self) -> u32 {
        self.slot_count
    }

    pub fn as_ptr(&self) -> *mut u8 {
        self.ptr.as_ptr()
    }

    pub fn slot_ptr(&self, i: u32) -> Option<*mut u8> {
        if i >= self.slot_count {
            return None;
        }
        // SAFETY: i is in range, so the offset is inside the mapping.
        Some(unsafe { self.ptr.as_ptr().add(i as usize * self.slot_size as usize) })
    }

    /// # Safety
    /// No op naming slot `i` may be in flight: the kernel writes there from a
    /// kworker.
    pub unsafe fn slot(&self, i: u32) -> &[u8] {
        let p = self.slot_ptr(i).expect("slot index out of range");
        // SAFETY: caller guarantees no concurrent kernel write.
        unsafe { std::slice::from_raw_parts(p, self.slot_size as usize) }
    }

    /// # Safety
    /// As [`Arena::slot`], and no other reference to this slot may exist.
    #[allow(clippy::mut_from_ref)]
    pub unsafe fn slot_mut(&self, i: u32) -> &mut [u8] {
        let p = self.slot_ptr(i).expect("slot index out of range");
        // SAFETY: caller guarantees exclusivity.
        unsafe { std::slice::from_raw_parts_mut(p, self.slot_size as usize) }
    }

    pub fn madvise(&self, advice: i32) -> io::Result<()> {
        // SAFETY: our own mapping, with its real length.
        let r = unsafe { sys::madvise(self.ptr.as_ptr().cast(), self.len, advice) };
        if r < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    /// Unmap explicitly, reporting the errno.
    pub fn unmap(self) -> io::Result<()> {
        let me = ManuallyDrop::new(self);
        // SAFETY: our own mapping; Drop is suppressed so it happens once.
        let r = unsafe { sys::munmap(me.ptr.as_ptr().cast(), me.len) };
        if r < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }
}

impl Drop for Arena {
    fn drop(&mut self) {
        // SAFETY: our own mapping.
        unsafe { sys::munmap(self.ptr.as_ptr().cast(), self.len) };
    }
}

// ---------------------------------------------------------------------------
// SQE constructors
// ---------------------------------------------------------------------------
//
// Each zeroes everything its opcode does not read, because a field an opcode
// ignores must be zero.

impl Sqe {
    pub fn nop(user_data: u64) -> Sqe {
        Sqe {
            opcode: KORU_OP_NOP,
            user_data,
            ..Sqe::default()
        }
    }

    pub fn delay_ns(user_data: u64, ns: u64) -> Sqe {
        Sqe {
            opcode: KORU_OP_DELAY_NS,
            off: ns,
            user_data,
            ..Sqe::default()
        }
    }

    pub fn checksum(user_data: u64, slot: u32, off: u64, len: u32) -> Sqe {
        Sqe {
            opcode: KORU_OP_CHECKSUM,
            len,
            off,
            user_data,
            slot,
            ..Sqe::default()
        }
    }

    /// `handle` carries the open flags, not a handle.
    pub fn open(user_data: u64, slot: u32, off: u64, len: u32, flags: u32) -> Sqe {
        Sqe {
            opcode: KORU_OP_OPEN,
            len,
            off,
            user_data,
            slot,
            handle: flags,
            ..Sqe::default()
        }
    }

    /// `off` is a file offset; the destination is always slot offset 0.
    pub fn read(user_data: u64, handle: u32, slot: u32, off: u64, len: u32) -> Sqe {
        Sqe {
            opcode: KORU_OP_READ,
            len,
            off,
            user_data,
            slot,
            handle,
            ..Sqe::default()
        }
    }

    /// `len` carries the koru event mask; `off` and `slot` must be zero.
    pub fn poll_add(user_data: u64, handle: u32, events: u32) -> Sqe {
        Sqe {
            opcode: KORU_OP_POLL_ADD,
            len: events,
            user_data,
            handle,
            ..Sqe::default()
        }
    }

    /// `off` is a within-slot offset and must be a multiple of 8; `len` is the
    /// caller's buffer size, which the kernel clamps to `size_of::<KoruStat>()`.
    pub fn stat(user_data: u64, handle: u32, slot: u32, off: u64, len: u32) -> Sqe {
        Sqe {
            opcode: KORU_OP_STAT,
            len,
            off,
            user_data,
            slot,
            handle,
            ..Sqe::default()
        }
    }

    /// A path op: the path is `len` bytes at `off`, and the opcode's argument
    /// follows it in the slot. `handle` must be zero.
    pub fn path(opcode: u8, user_data: u64, slot: u32, off: u64, len: u32) -> Sqe {
        Sqe {
            opcode,
            len,
            off,
            user_data,
            slot,
            ..Sqe::default()
        }
    }

    /// `off` carries the descriptor; every other field must be zero.
    pub fn adopt_fd(user_data: u64, fd: i32) -> Sqe {
        Sqe {
            opcode: KORU_OP_ADOPT_FD,
            off: fd as u64,
            user_data,
            ..Sqe::default()
        }
    }

    /// `off` is a file offset; the source is always slot offset 0.
    pub fn write(user_data: u64, handle: u32, slot: u32, off: u64, len: u32) -> Sqe {
        Sqe {
            opcode: KORU_OP_WRITE,
            len,
            off,
            user_data,
            slot,
            handle,
            ..Sqe::default()
        }
    }

    pub fn close(user_data: u64, handle: u32) -> Sqe {
        Sqe {
            opcode: KORU_OP_CLOSE,
            user_data,
            handle,
            ..Sqe::default()
        }
    }

    /// `off` is the target's `user_data`.
    pub fn cancel(user_data: u64, target: u64) -> Sqe {
        Sqe {
            opcode: KORU_OP_CANCEL,
            off: target,
            user_data,
            ..Sqe::default()
        }
    }
}

impl KoruStat {
    /// Decode a whole `KoruStat` out of a slot. `None` if there is not one
    /// there, which is what a `len` short of the struct leaves behind.
    pub fn read_from(bytes: &[u8]) -> Option<KoruStat> {
        if bytes.len() < size_of::<KoruStat>() {
            return None;
        }
        // SAFETY: every bit pattern is a valid `KoruStat`, the length is
        // checked, and an unaligned read is allowed for.
        Some(unsafe { std::ptr::read_unaligned(bytes.as_ptr().cast()) })
    }
}

impl KoruTimes {
    pub fn as_bytes(&self) -> [u8; size_of::<KoruTimes>()] {
        // SAFETY: `repr(C)`, integers only, no padding.
        unsafe { std::mem::transmute_copy(self) }
    }
}

/// Where a path op's argument goes: the first 8-aligned offset at or after the
/// end of the path. The kernel computes the same thing.
pub const fn arg_offset(off: u64, len: u32) -> u64 {
    (off + len as u64 + 7) & !7
}
