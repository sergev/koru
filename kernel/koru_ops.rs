// SPDX-License-Identifier: GPL-2.0

//! Opcode dispatch, SQE validation, the deferred-op work item, and the raw
//! `bindings::` calls that back `OPEN`. See `doc/Notes.md`.

use kernel::{
    bindings,
    error::from_err_ptr,
    fs::File,
    impl_has_delayed_work, new_delayed_work,
    page::PAGE_SIZE,
    prelude::*,
    str::CStrExt,
    sync::{aref::ARef, Arc, ArcBorrow},
    time::{msecs_to_jiffies, Jiffies},
    workqueue::{self, DelayedWork, WorkItem},
};

use core::ptr::NonNull;

use crate::koru_abi::*;
use crate::{module_get_live, module_put, RingCtx};

/// Nanoseconds to jiffies, rounded up so a sub-millisecond delay still waits.
fn delay_jiffies(ns: u64) -> Jiffies {
    msecs_to_jiffies(ns.div_ceil(1_000_000).try_into().unwrap_or(u32::MAX))
}

/// A deferred operation. doc/Notes.md's Lifetimes diagram is the shape.
///
/// Holds its own `Arc<RingCtx>`, so the ring outlives `close(fd)` while work is
/// still queued and the last one out frees it.
///
/// Also holds a module reference for its whole life, including the delay before
/// it runs: a queued work item points at `run` in module text, and nothing else
/// keeps that text alive once the fd is closed.
#[pin_data(PinnedDrop)]
pub(crate) struct OpWork {
    #[pin]
    work: DelayedWork<OpWork>,
    ring: Arc<RingCtx>,
    sqe: Sqe,
}

#[pinned_drop]
impl PinnedDrop for OpWork {
    fn drop(self: Pin<&mut Self>) {
        module_put();
    }
}

impl_has_delayed_work! {
    impl HasDelayedWork<Self> for OpWork { self.work }
}

impl WorkItem for OpWork {
    type Pointer = Arc<OpWork>;

    /// Runs in a kworker: no user memory, and the submitting task may be gone.
    fn run(this: Arc<OpWork>) {
        let sqe = &this.sqe;
        let (res, slot) = match sqe.opcode {
            KORU_OP_DELAY_NS => (0, None),
            KORU_OP_CHECKSUM => (
                this.ring
                    .checksum(sqe)
                    .unwrap_or_else(|e| i64::from(e.to_errno())),
                Some(sqe.slot),
            ),
            _ => (i64::from(EINVAL.to_errno()), None),
        };
        this.ring.complete(RingCtx::cqe(sqe, res), slot);
    }
}

impl RingCtx {
    /// Opcode dispatch. `Some(res)` completed inline, `None` was deferred and
    /// will post its own completion. Never fails the ioctl: per E1 a bad SQE is
    /// a completion.
    pub(crate) fn dispatch(me: ArcBorrow<'_, RingCtx>, sqe: &Sqe) -> Option<i64> {
        let einval = i64::from(EINVAL.to_errno());

        if sqe.rsvd0 != 0 || sqe.flags & !KORU_SQE_FLAGS_ALL != 0 {
            return Some(einval);
        }

        match sqe.opcode {
            KORU_OP_NOP => {
                // NOP reads no argument fields, so all must be zero.
                if sqe.len != 0 || sqe.off != 0 || sqe.slot != 0 || sqe.handle != 0 {
                    return Some(einval);
                }
                Some(0)
            }
            KORU_OP_DELAY_NS => {
                // DELAY_NS reads only `off`.
                if sqe.len != 0 || sqe.slot != 0 || sqe.handle != 0 {
                    return Some(einval);
                }
                RingCtx::defer(me, sqe, delay_jiffies(sqe.off))
            }
            // Inline, in the submitting task's context: a kworker would resolve
            // and permission-check as root. Both must return `Some`, and
            // neither may call `complete`, which belongs to the deferred path.
            KORU_OP_OPEN => Some(me.open_op(sqe)),
            KORU_OP_CLOSE => Some(me.close_op(sqe)),
            KORU_OP_CHECKSUM => {
                if sqe.handle != 0 {
                    return Some(einval);
                }
                // Validate in ioctl context, before claiming anything.
                if let Err(e) = RingCtx::check_range(&me, sqe) {
                    return Some(i64::from(e.to_errno()));
                }
                if !me.state.lock().slot_try_acquire(sqe.slot) {
                    return Some(i64::from(EBUSY.to_errno()));
                }
                // Deferred, so the slot is genuinely held across a window. That
                // window is what makes exclusivity observable, and it is the
                // same shape READ takes in T10.
                match RingCtx::defer(me, sqe, 0) {
                    None => None,
                    Some(res) => {
                        me.state.lock().slot_release(sqe.slot);
                        Some(res)
                    }
                }
            }
            // Unknown, or defined but not yet implemented.
            _ => Some(einval),
        }
    }

    /// Bounds-check `slot`, `off` and `len` against the configured arena.
    fn check_range(&self, sqe: &Sqe) -> Result<()> {
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };
        if sqe.slot >= cfg.slot_count {
            return Err(EINVAL);
        }
        // All user-controlled: check in u64 before deriving any page index.
        let end = u64::from(sqe.len).checked_add(sqe.off).ok_or(EINVAL)?;
        if end > u64::from(cfg.slot_size) {
            return Err(EINVAL);
        }
        Ok(())
    }

    /// Byte offset of `sqe.off` within slot `sqe.slot`. Caller has already run
    /// [`check_range`](Self::check_range).
    fn slot_offset(&self, sqe: &Sqe) -> Result<usize> {
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };
        let base = u64::from(sqe.slot)
            .checked_mul(u64::from(cfg.slot_size))
            .ok_or(EINVAL)?
            .checked_add(sqe.off)
            .ok_or(EINVAL)?;
        usize::try_from(base).map_err(|_| EINVAL)
    }

    /// Copy `len` bytes out of the arena at `pos` into `out`.
    ///
    /// A single snapshot; the caller holds the slot, which is what satisfies
    /// `read_raw`'s no-concurrent-access precondition.
    fn read_slot(&self, mut pos: usize, len: usize, out: &mut KVec<u8>) -> Result<()> {
        let mut left = len;
        let mut buf = [0u8; 256];
        let arena = self.arena.lock();

        while left > 0 {
            let page = arena.pages.get(pos / PAGE_SIZE).ok_or(EINVAL)?;
            let in_page = pos % PAGE_SIZE;
            let n = core::cmp::min(core::cmp::min(left, PAGE_SIZE - in_page), buf.len());

            // SAFETY: `buf` is valid for `n` bytes and `in_page + n <= PAGE_SIZE`.
            unsafe { page.read_raw(buf.as_mut_ptr(), in_page, n)? };

            out.extend_from_slice(&buf[..n], GFP_KERNEL)?;
            pos += n;
            left -= n;
        }
        Ok(())
    }

    /// FNV-1a over `len` bytes at `off` in slot `slot`.
    ///
    /// Masked to 63 bits so the result is never mistaken for an errno. Streams
    /// rather than buffering: `len` can be a whole slot.
    pub(crate) fn checksum(&self, sqe: &Sqe) -> Result<i64> {
        self.check_range(sqe)?;
        let mut pos = self.slot_offset(sqe)?;
        let mut left = sqe.len as usize;

        let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
        let mut buf = [0u8; 256];
        let arena = self.arena.lock();

        while left > 0 {
            let page = arena.pages.get(pos / PAGE_SIZE).ok_or(EINVAL)?;
            let in_page = pos % PAGE_SIZE;
            let n = core::cmp::min(core::cmp::min(left, PAGE_SIZE - in_page), buf.len());

            // SAFETY: `buf` is valid for `n` bytes and `in_page + n <= PAGE_SIZE`.
            // The slot is claimed for the life of this op, so nothing else
            // touches the page.
            unsafe { page.read_raw(buf.as_mut_ptr(), in_page, n)? };

            for &b in &buf[..n] {
                hash ^= u64::from(b);
                hash = hash.wrapping_mul(0x100_0000_01b3);
            }
            pos += n;
            left -= n;
        }

        Ok((hash & 0x7fff_ffff_ffff_ffff) as i64)
    }

    /// `OPEN`: the path is `len` bytes at `off` in slot `slot`, and `handle`
    /// carries the `KORU_O_*` flags. Returns the new handle in `res`.
    fn open_op(&self, sqe: &Sqe) -> i64 {
        match self.do_open(sqe) {
            Ok(handle) => i64::from(handle),
            Err(e) => i64::from(e.to_errno()),
        }
    }

    fn do_open(&self, sqe: &Sqe) -> Result<u32> {
        let flags = open_flags(sqe.handle)?;

        self.check_range(sqe)?;
        // Notes' second clamp: check_range only bounds by slot_size.
        if sqe.len == 0 || sqe.len >= bindings::PATH_MAX {
            return Err(EINVAL);
        }
        let pos = self.slot_offset(sqe)?;

        // Claim the slot so nothing writes the page mid-copy, and release it as
        // soon as the snapshot is taken: filp_open can block on disk.
        if !self.state.lock().slot_try_acquire(sqe.slot) {
            return Err(EBUSY);
        }
        let mut path = KVec::with_capacity(sqe.len as usize + 1, GFP_KERNEL)?;
        let copied = self.read_slot(pos, sqe.len as usize, &mut path);
        self.state.lock().slot_release(sqe.slot);
        copied?;

        // Terminate our own copy, then scan it. Never strlen in place, and
        // never hand a pointer into a mapped page to the VFS.
        path.push(0u8, GFP_KERNEL)?;
        let cpath = CStr::from_bytes_with_nul(&path).map_err(|_| EINVAL)?;

        // SAFETY: `cpath` is NUL-terminated and lives across the call. Runs in
        // the submitting task, so creds and namespaces are the caller's.
        let ptr = from_err_ptr(unsafe { bindings::filp_open(cpath.as_char_ptr(), flags, 0) })?;
        let ptr = NonNull::new(ptr.cast::<File>()).ok_or(EINVAL)?;
        // SAFETY: `filp_open` returned a reference and we take ownership of it.
        let file = unsafe { ARef::from_raw(ptr) };

        // Bound first: a guard in the scrutinee would live across the arms, so
        // the fput below would run with the table locked.
        let inserted = self.handles.lock().insert(file);
        match inserted {
            Ok(handle) => Ok(handle),
            Err(file) => {
                drop(file);
                Err(EMFILE)
            }
        }
    }

    /// `CLOSE`: retire `handle`. `len`, `off` and `slot` must be zero.
    fn close_op(&self, sqe: &Sqe) -> i64 {
        if sqe.len != 0 || sqe.off != 0 || sqe.slot != 0 {
            return i64::from(EINVAL.to_errno());
        }
        // Bound first, so the fput happens after the guard is dropped.
        let closed = self.handles.lock().close(sqe.handle);
        match closed {
            Ok(file) => {
                drop(file);
                0
            }
            Err(e) => i64::from(e.to_errno()),
        }
    }

    /// Queue an op on the workqueue. `None` once it owns the reservation.
    fn defer(me: ArcBorrow<'_, RingCtx>, sqe: &Sqe, jiffies: Jiffies) -> Option<i64> {
        // Paired with `PinnedDrop for OpWork`. Taken before the allocation so
        // the drop impl always has a reference to release.
        module_get_live();

        let op = match Arc::pin_init(
            pin_init!(OpWork {
                work <- new_delayed_work!("OpWork::work"),
                ring: Arc::from(me),
                sqe: *sqe,
            }),
            GFP_KERNEL,
        ) {
            Ok(op) => op,
            // C1 still holds: a failed op is a completion, not an ioctl error.
            Err(e) => {
                module_put();
                return Some(i64::from(e.to_errno()));
            }
        };

        // Counted before enqueueing, or `run` could decrement first.
        me.state.lock().inflight += 1;
        if workqueue::system().enqueue_delayed(op, jiffies).is_err() {
            me.state.lock().inflight -= 1;
            return Some(i64::from(EAGAIN.to_errno()));
        }
        None
    }
}

/// Translate koru's open flags to the host's. Rejects unknown bits and the
/// non-existent access mode 3.
fn open_flags(flags: u32) -> Result<i32> {
    if flags & !KORU_OPEN_FLAGS_ALL != 0 {
        return Err(EINVAL);
    }
    let mut out = match flags & KORU_O_ACCMODE {
        KORU_O_WRONLY => bindings::O_WRONLY,
        KORU_O_RDWR => bindings::O_RDWR,
        KORU_O_ACCMODE => return Err(EINVAL),
        _ => bindings::O_RDONLY,
    };
    if flags & KORU_O_NOFOLLOW != 0 {
        out |= bindings::O_NOFOLLOW;
    }
    if flags & KORU_O_DIRECTORY != 0 {
        out |= bindings::O_DIRECTORY;
    }
    // Always: kernel opens of regular files, with no controlling terminal.
    out |= bindings::O_LARGEFILE | bindings::O_NOCTTY;
    i32::try_from(out).map_err(|_| EINVAL)
}
