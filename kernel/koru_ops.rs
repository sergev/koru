// SPDX-License-Identifier: GPL-2.0

//! Opcode dispatch, SQE validation, the deferred-op work item, and the raw
//! `bindings::` calls that back `OPEN`. See `doc/Notes.md`.

use kernel::{
    bindings,
    cred::Credential,
    error::from_err_ptr,
    ffi,
    fs::{File, LocalFile},
    impl_has_delayed_work, new_delayed_work,
    page::PAGE_SIZE,
    prelude::*,
    str::CStrExt,
    sync::{
        aref::ARef,
        atomic::{Atomic, Full, Relaxed},
        Arc, ArcBorrow,
    },
    time::{msecs_to_jiffies, Jiffies},
    transmute::{AsBytes, FromBytes},
    types::Opaque,
    workqueue::{self, DelayedWork, HasWork, Work, WorkItem},
};

use core::mem::offset_of;
use core::ptr::NonNull;

use crate::koru_abi::*;
use crate::koru_path::{Link, Lookup, LOOKUP_FOLLOW};
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
    /// Resolved at submit time, so a `CLOSE` racing this op cannot free it.
    /// Dropped with the work item, whether it ran or not.
    file: Option<ARef<File>>,
    /// The submitter's credentials, and through them its user namespace.
    /// `current_cred()` in a kworker is `init_cred`; see doc/Notes.md.
    cred: ARef<Credential>,
    /// `POLL_ADD`'s arm. Idle in every other op, which costs them the space
    /// rather than a second allocation; see doc/Notes.md.
    #[pin]
    poll: PollState,
}

/// The armed half of a `POLL_ADD`. Four parties reach it and `token` makes
/// exactly one of them the owner of the teardown; doc/Notes.md says why.
#[pin_data]
pub(crate) struct PollState {
    /// On the file's waitqueue while armed. Pinned in the `Arc`, so the
    /// callback can find the op from the entry.
    #[pin]
    wait: Opaque<bindings::wait_queue_entry>,
    /// The head it went on, or null. Written during the arm.
    head: Atomic<*mut bindings::wait_queue_head>,
    /// One-shot, `POLL_ARMED` until somebody wins it.
    token: Atomic<u32>,
    /// What the wake saw, 0 when it was woken with no key.
    fired: Atomic<u32>,
    /// The kernel-side mask asked for. Immutable after the arm.
    interest: u32,
}

// SAFETY: the entry is touched only under the waitqueue head's own spinlock,
// which `add_wait_queue`, `remove_wait_queue` and the wake callback all hold;
// everything else here is atomic or immutable after the arm.
unsafe impl Send for PollState {}
// SAFETY: as above.
unsafe impl Sync for PollState {}

/// Nobody owns the teardown yet.
const POLL_ARMED: u32 = 0;
/// Somebody does, and it is theirs alone.
const POLL_TAKEN: u32 = 1;

impl PollState {
    fn new(interest: u32) -> impl PinInit<PollState> {
        pin_init!(PollState {
            // `init_waitqueue_func_entry` plus `INIT_LIST_HEAD`, both inlines.
            wait <- Opaque::ffi_init(|p: *mut bindings::wait_queue_entry| {
                // SAFETY: `p` points at our own uninitialised storage.
                unsafe {
                    (*p).flags = 0;
                    (*p).private = core::ptr::null_mut();
                    (*p).func = Some(poll_wake);
                    let list = &raw mut (*p).entry;
                    (*list).next = list;
                    (*list).prev = list;
                }
            }),
            head: Atomic::new(core::ptr::null_mut()),
            token: Atomic::new(POLL_ARMED),
            fired: Atomic::new(0),
            interest,
        })
    }
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

impl OpWork {
    pub(crate) fn user_data(&self) -> u64 {
        self.sqe.user_data
    }

    /// The slot this op owns while deferred, released when it completes.
    /// `CANCEL` must free exactly what `run` would have.
    fn held_slot(sqe: &Sqe) -> Option<u32> {
        match sqe.opcode {
            KORU_OP_CHECKSUM | KORU_OP_READ | KORU_OP_WRITE | KORU_OP_STAT => Some(sqe.slot),
            _ => None,
        }
    }

    /// The `struct work_struct` inside this op, for `queue_work_on`.
    fn work_struct(op: *const OpWork) -> *mut bindings::work_struct {
        // SAFETY: `op` points at a live `OpWork`, so its `work` field is live.
        let work = unsafe { <OpWork as HasWork<OpWork>>::raw_get_work(op.cast_mut()) };
        // SAFETY: as above.
        unsafe { Work::raw_get(work) }
    }

    /// The `struct delayed_work` inside this op, for `cancel_delayed_work`.
    fn delayed_work(op: &Arc<OpWork>) -> *mut bindings::delayed_work {
        let work = OpWork::work_struct(Arc::as_ptr(op));
        // SAFETY: `work` is the `work` field of a `DelayedWork`, which is
        // `repr(transparent)` over `bindings::delayed_work`.
        unsafe { kernel::container_of!(work, bindings::delayed_work, work) }
    }

    /// Offset of the waitqueue entry, so the wake callback can find the op.
    const WAIT_OFFSET: usize = offset_of!(OpWork, poll) + offset_of!(PollState, wait);

    /// Win the one-shot token. The winner owns the disarm, the enqueue credit
    /// and the completion.
    fn poll_claim(&self) -> bool {
        self.poll
            .token
            .cmpxchg(POLL_ARMED, POLL_TAKEN, Full)
            .is_ok()
    }

    /// Take the entry off the file's waitqueue.
    ///
    /// # Safety
    ///
    /// Process context only, and only by the token's winner, once.
    unsafe fn poll_disarm(&self) {
        let head = self.poll.head.load(Relaxed);
        if !head.is_null() {
            // SAFETY: the op holds an `ARef<File>`, so the head it was queued
            // on is still alive; the entry is ours and is still on that list.
            unsafe { bindings::remove_wait_queue(head, self.poll.wait.get()) };
        }
    }
}

/// The waitqueue callback. **Runs with the head's spinlock held, and for a
/// socket in softirq**, so it takes no koru lock, completes nothing, drops
/// nothing and touches no list: token, mask, enqueue. See doc/Notes.md.
unsafe extern "C" fn poll_wake(
    entry: *mut bindings::wait_queue_entry,
    _mode: kernel::ffi::c_uint,
    _flags: kernel::ffi::c_int,
    key: *mut kernel::ffi::c_void,
) -> kernel::ffi::c_int {
    // SAFETY: the entry is on a queue only between the arm and the disarm, and
    // for that window the registry and the credit both keep the op alive.
    let op = unsafe { &*entry.byte_sub(OpWork::WAIT_OFFSET).cast::<OpWork>() };

    // A keyless wake says nothing about what happened, so it always counts.
    let mask = key as usize as u32;
    if mask != 0 && mask & (op.poll.interest | POLL_ALWAYS) == 0 {
        return 0;
    }
    if !op.poll_claim() {
        return 0;
    }
    op.poll.fired.store(mask, Relaxed);

    // Cannot fail: the token admits one caller and the work was never queued.
    // `run` reclaims the credit the arm leaked.
    // SAFETY: the op is alive as above, and its work was initialised with
    // `run` as its function when it was allocated.
    unsafe {
        bindings::queue_work_on(
            bindings::wq_misc_consts_WORK_CPU_UNBOUND as kernel::ffi::c_int,
            bindings::system_wq,
            OpWork::work_struct(op),
        );
    }
    1
}

/// The poll table, with our own fields after it. `_qproc` is called
/// synchronously inside `f_op->poll`, so the arm's stack frame is enough.
#[repr(C)]
struct KoruPollTable {
    pt: bindings::poll_table_struct,
    op: *const OpWork,
    heads: u32,
}

/// Queue the op on the head the file names. One waitqueue only: a second is
/// counted and refused, rather than growing io_uring's double-entry
/// machinery. A pipe opened read-write is the reachable case.
unsafe extern "C" fn poll_queue_proc(
    _file: *mut bindings::file,
    head: *mut bindings::wait_queue_head,
    pt: *mut bindings::poll_table_struct,
) {
    // SAFETY: `pt` is the first field of the `KoruPollTable` the arm made.
    let table = pt.cast::<KoruPollTable>();
    // SAFETY: as above, and the arm owns it for the whole call.
    unsafe {
        (*table).heads += 1;
        if (*table).heads > 1 {
            return;
        }
        let op = (*table).op;
        (*op).poll.head.store(head, Relaxed);
        // The entry is initialised and on no list, and the op holds an
        // `ARef<File>`, so the head outlives the arm.
        bindings::add_wait_queue(head, (*op).poll.wait.get());
    }
}

impl WorkItem for OpWork {
    type Pointer = Arc<OpWork>;

    /// Runs in a kworker: no user memory, and the submitting task may be gone.
    fn run(this: Arc<OpWork>) {
        let sqe = &this.sqe;
        // `STAT` is the first opcode to set `extra`, so every arm carries one.
        let failed = |e: Error| (i64::from(e.to_errno()), 0u64);
        let (res, extra) = match sqe.opcode {
            KORU_OP_DELAY_NS => (0, 0),
            KORU_OP_CHECKSUM => this.ring.checksum(sqe).map_or_else(failed, |r| (r, 0)),
            KORU_OP_READ => this
                .ring
                .do_read(sqe, this.file.as_deref())
                .map_or_else(failed, |r| (r, 0)),
            KORU_OP_WRITE => this
                .ring
                .do_write(sqe, this.file.as_deref())
                .map_or_else(failed, |r| (r, 0)),
            KORU_OP_STAT => this
                .ring
                .do_stat(sqe, this.file.as_deref(), &this.cred)
                .unwrap_or_else(failed),
            // Only a fired poll reaches here, once: the callback won the token
            // before queueing. Off the waitqueue first.
            KORU_OP_POLL_ADD => {
                // SAFETY: the callback won the token, so this is the one
                // disarm, and a kworker is process context.
                unsafe { this.poll_disarm() };
                let fired = this.poll.fired.load(Relaxed);
                // A keyless wake leaves nothing to report, so ask the file.
                let mask = if fired != 0 {
                    fired
                } else {
                    // SAFETY: the file is alive for as long as the op is.
                    unsafe { poll_mask_now(&this) }
                };
                (
                    i64::from(poll_to_koru(mask & (this.poll.interest | POLL_ALWAYS))),
                    0,
                )
            }
            _ => (i64::from(EINVAL.to_errno()), 0),
        };
        this.ring
            .complete(RingCtx::cqe_extra(sqe, res, extra), OpWork::held_slot(sqe));

        // Unregister *after* completing, so a `CANCEL` arriving while this ran
        // still finds the entry and reports `EALREADY` rather than `ENOENT`.
        // This also drops the registry's reference, breaking the cycle.
        RingCtx::pending_remove(&mut this.ring.pending.lock(), &this);
    }
}

impl RingCtx {
    /// Opcode dispatch. `Some((res, extra))` completed inline, `None` was
    /// deferred and will post its own completion. Never fails the ioctl: per E1
    /// a bad SQE is a completion. The tuple mirrors `run`'s, because `STATX_AT`
    /// is the first inline op that sets `extra`.
    pub(crate) fn dispatch(
        me: ArcBorrow<'_, RingCtx>,
        ring: &File,
        sqe: &Sqe,
    ) -> Option<(i64, u64)> {
        let einval = i64::from(EINVAL.to_errno());
        // Only `STATX_AT` has anything to put in `extra`.
        let plain = |res: i64| Some((res, 0));

        if sqe.rsvd0 != 0 || sqe.flags & !KORU_SQE_FLAGS_ALL != 0 {
            return plain(einval);
        }

        match sqe.opcode {
            KORU_OP_NOP => {
                // NOP reads no argument fields, so all must be zero.
                if sqe.len != 0 || sqe.off != 0 || sqe.slot != 0 || sqe.handle != 0 {
                    return plain(einval);
                }
                plain(0)
            }
            KORU_OP_DELAY_NS => {
                // DELAY_NS reads only `off`.
                if sqe.len != 0 || sqe.slot != 0 || sqe.handle != 0 {
                    return plain(einval);
                }
                // A delay pins a CQ reservation for its whole duration.
                if sqe.off > KORU_MAX_DELAY_NS {
                    return plain(einval);
                }
                RingCtx::defer(me, sqe, delay_jiffies(sqe.off), None).map(|res| (res, 0))
            }
            // Inline, in the submitting task's context: a kworker would resolve
            // and permission-check as root. Both must return `Some`, and
            // neither may call `complete`, which belongs to the deferred path.
            KORU_OP_OPEN => plain(me.open_op(sqe)),
            KORU_OP_CLOSE => plain(me.close_op(sqe)),
            KORU_OP_READ => RingCtx::read_op(me, sqe).map(|res| (res, 0)),
            KORU_OP_WRITE => RingCtx::write_op(me, sqe).map(|res| (res, 0)),
            KORU_OP_ADOPT_FD => plain(me.adopt_op(ring, sqe)),
            KORU_OP_POLL_ADD => RingCtx::poll_op(me, sqe).map(|res| (res, 0)),
            KORU_OP_STAT => RingCtx::stat_op(me, sqe).map(|res| (res, 0)),
            // Inline for `OPEN`'s reason, and permanently. See `path_op`.
            KORU_OP_TRUNCATE | KORU_OP_UTIMES | KORU_OP_READLINK | KORU_OP_STATX_AT => {
                Some(me.path_op(sqe))
            }
            KORU_OP_CANCEL => plain(me.cancel_op(sqe)),
            KORU_OP_CHECKSUM => {
                if sqe.handle != 0 {
                    return plain(einval);
                }
                // Validate in ioctl context, before claiming anything.
                if let Err(e) = RingCtx::check_range(&me, sqe) {
                    return plain(i64::from(e.to_errno()));
                }
                if !me.state.lock().slot_try_acquire(sqe.slot) {
                    return plain(i64::from(EBUSY.to_errno()));
                }
                // Deferred, so the slot is genuinely held across a window. That
                // window is what makes exclusivity observable, and it is the
                // same shape READ takes in T10.
                RingCtx::defer_holding_slot(me, sqe, None).map(|res| (res, 0))
            }
            // Unknown, or defined but not yet implemented.
            _ => plain(einval),
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
        self.slot_pos(sqe.slot, sqe.off)
    }

    /// Byte offset of `off` within `slot`. Caller has bounds-checked both.
    fn slot_pos(&self, slot: u32, off: u64) -> Result<usize> {
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };
        let base = u64::from(slot)
            .checked_mul(u64::from(cfg.slot_size))
            .ok_or(EINVAL)?
            .checked_add(off)
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

    /// Bounds-check a path argument, returning its arena offset. Before any
    /// claim: `slot_try_acquire` does not bounds-check its own index.
    fn path_validate(&self, sqe: &Sqe) -> Result<usize> {
        self.check_range(sqe)?;
        // Notes' second clamp: check_range only bounds by slot_size.
        if sqe.len == 0 || sqe.len >= bindings::PATH_MAX {
            return Err(EINVAL);
        }
        self.slot_offset(sqe)
    }

    /// Copy a path out of an already-claimed slot, terminate our own copy and
    /// reject an embedded NUL. Never strlen in place, and never hand the VFS a
    /// pointer into a mapped page.
    fn copy_path(&self, pos: usize, len: usize) -> Result<KVec<u8>> {
        let mut path = KVec::with_capacity(len + 1, GFP_KERNEL)?;
        self.read_slot(pos, len, &mut path)?;
        path.push(0u8, GFP_KERNEL)?;
        CStr::from_bytes_with_nul(&path).map_err(|_| EINVAL)?;
        Ok(path)
    }

    /// Where a path op's argument block starts: the first 8-aligned slot offset
    /// at or after the end of the path. `EINVAL` if `size` bytes do not fit.
    fn arg_offset(&self, sqe: &Sqe, size: usize) -> Result<u64> {
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };
        let end = sqe.off.checked_add(u64::from(sqe.len)).ok_or(EINVAL)?;
        let at = end.checked_add(7).ok_or(EINVAL)? & !7u64;
        if at.checked_add(size as u64).ok_or(EINVAL)? > u64::from(cfg.slot_size) {
            return Err(EINVAL);
        }
        Ok(at)
    }

    /// One claim over a path op's path and the argument after it, so the two
    /// halves are one snapshot.
    fn path_and_arg(&self, sqe: &Sqe, size: usize) -> Result<(KVec<u8>, KVec<u8>)> {
        // Every path op names its file by path alone.
        if sqe.handle != 0 {
            return Err(EINVAL);
        }
        let pos = self.path_validate(sqe)?;
        let apos = self.slot_pos(sqe.slot, self.arg_offset(sqe, size)?)?;

        if !self.state.lock().slot_try_acquire(sqe.slot) {
            return Err(EBUSY);
        }
        let mut arg = KVec::new();
        let taken = self
            .copy_path(pos, sqe.len as usize)
            .and_then(|path| self.read_slot(apos, size, &mut arg).map(|()| path));
        self.state.lock().slot_release(sqe.slot);
        Ok((taken?, arg))
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
        let pos = self.path_validate(sqe)?;

        // Claim the slot so nothing writes the page mid-copy, and release it as
        // soon as the snapshot is taken: filp_open can block on disk.
        if !self.state.lock().slot_try_acquire(sqe.slot) {
            return Err(EBUSY);
        }
        let path = self.copy_path(pos, sqe.len as usize);
        self.state.lock().slot_release(sqe.slot);
        let path = path?;
        let cpath = CStr::from_bytes_with_nul(&path).map_err(|_| EINVAL)?;

        // SAFETY: `cpath` is NUL-terminated and lives across the call. Runs in
        // the submitting task, so creds and namespaces are the caller's.
        let ptr = from_err_ptr(unsafe { bindings::filp_open(cpath.as_char_ptr(), flags, 0) })?;
        let ptr = NonNull::new(ptr.cast::<File>()).ok_or(EINVAL)?;
        // SAFETY: `filp_open` returned a reference and we take ownership of it.
        let file = unsafe { ARef::from_raw(ptr) };

        // No file-type gate here since T18: `check_readable` and
        // `check_writable` carry it, so a handle to a socket or a device is
        // harmless. `filp_open` ran with the caller's creds in the caller's
        // context, so it grants no authority the caller did not have.

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

    /// `READ`: deferred, with the file resolved and the slot claimed here, in
    /// ioctl context. `None` once the work item owns both.
    fn read_op(me: ArcBorrow<'_, RingCtx>, sqe: &Sqe) -> Option<i64> {
        let file = match RingCtx::read_validate(&me, sqe) {
            Ok(file) => file,
            Err(e) => return Some(i64::from(e.to_errno())),
        };
        if !me.state.lock().slot_try_acquire(sqe.slot) {
            return Some(i64::from(EBUSY.to_errno()));
        }
        RingCtx::defer_holding_slot(me, sqe, Some(file))
    }

    /// Everything `READ` can reject before it costs a work item.
    fn read_validate(&self, sqe: &Sqe) -> Result<ARef<File>> {
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };
        if sqe.slot >= cfg.slot_count {
            return Err(EINVAL);
        }
        // Reads land at slot offset 0, so `len` alone has to fit.
        if sqe.len == 0 || sqe.len > cfg.slot_size {
            return Err(EINVAL);
        }
        // `loff_t` is signed; a negative offset is not a position.
        if sqe.off > i64::MAX as u64 {
            return Err(EINVAL);
        }

        // Bound first: a guard in the scrutinee would live across the arms.
        let file = self.handles.lock().resolve(sqe.handle)?;
        check_readable(&file)?;
        // As in `write_validate`: an unseekable file would ignore `off` rather
        // than refuse it, and silently ignoring a field is how a second
        // implementer loses a day.
        if sqe.off != 0 && !is_seekable(&file) {
            return Err(EINVAL);
        }
        Ok(file)
    }

    /// Runs in a kworker. Bounces through one page-sized buffer: `Page` exposes
    /// no lasting kernel address, and holding a `kmap_local_page` across a
    /// blocking read would inhibit migration for the whole I/O.
    fn do_read(&self, sqe: &Sqe, file: Option<&File>) -> Result<i64> {
        let file = file.ok_or(EINVAL)?;
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };

        // Slots are whole pages, so this is page-aligned and a chunk never
        // straddles two pages.
        let base = u64::from(sqe.slot)
            .checked_mul(u64::from(cfg.slot_size))
            .ok_or(EINVAL)?;
        let mut pos = usize::try_from(base).map_err(|_| EINVAL)?;
        let mut fpos = sqe.off as i64;
        let len = sqe.len as usize;
        let mut done = 0usize;

        let mut bounce = KVec::with_capacity(PAGE_SIZE, GFP_KERNEL)?;
        bounce.resize(PAGE_SIZE, 0u8, GFP_KERNEL)?;

        while done < len {
            let in_page = pos % PAGE_SIZE;
            let want = core::cmp::min(len - done, PAGE_SIZE - in_page);

            // The arena lock must NOT be held here. `mmap` takes it under
            // `mmap_lock`, and a filesystem read takes `mmap_lock` under the
            // inode rwsem that `kernel_read` acquires — so holding it across
            // this call closes a genuine deadlock cycle. Lockdep found it.
            let n = match read_at(file, &mut bounce[..want], &mut fpos) {
                Ok(n) => n,
                // A partial read is a result, not a failure. Same convention as
                // the submit loop's short count.
                Err(e) => return if done == 0 { Err(e) } else { Ok(done as i64) },
            };
            if n == 0 {
                break; // EOF.
            }

            {
                let arena = self.arena.lock();
                let page = arena.pages.get(pos / PAGE_SIZE).ok_or(EINVAL)?;
                // SAFETY: `bounce` is valid for `n` bytes and `in_page + n <=
                // PAGE_SIZE`. The slot is claimed for the life of this op, so
                // nothing else touches the page.
                unsafe { page.write_raw(bounce.as_ptr(), in_page, n)? };
            }

            pos += n;
            done += n;
        }

        Ok(done as i64)
    }

    /// `WRITE`: deferred, mirroring `READ`. `None` once the work item owns the
    /// file and the slot.
    fn write_op(me: ArcBorrow<'_, RingCtx>, sqe: &Sqe) -> Option<i64> {
        let file = match RingCtx::write_validate(&me, sqe) {
            Ok(file) => file,
            Err(e) => return Some(i64::from(e.to_errno())),
        };
        if !me.state.lock().slot_try_acquire(sqe.slot) {
            return Some(i64::from(EBUSY.to_errno()));
        }
        RingCtx::defer_holding_slot(me, sqe, Some(file))
    }

    /// Everything `WRITE` can reject before it costs a work item.
    fn write_validate(&self, sqe: &Sqe) -> Result<ARef<File>> {
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };
        if sqe.slot >= cfg.slot_count {
            return Err(EINVAL);
        }
        // Writes start at slot offset 0, so `len` alone has to fit.
        if sqe.len == 0 || sqe.len > cfg.slot_size {
            return Err(EINVAL);
        }
        // `loff_t` is signed; a negative offset is not a position.
        if sqe.off > i64::MAX as u64 {
            return Err(EINVAL);
        }

        let file = self.handles.lock().resolve(sqe.handle)?;
        check_writable(&file)?;
        // `rw_verify_area` rejects only a negative offset, so an unseekable
        // file would ignore `off` rather than refuse it. Refuse it here.
        if sqe.off != 0 && !is_seekable(&file) {
            return Err(EINVAL);
        }
        Ok(file)
    }

    /// Runs in a kworker. `do_read`'s loop with the copy reversed: out of the
    /// arena into the bounce buffer, then into the file.
    fn do_write(&self, sqe: &Sqe, file: Option<&File>) -> Result<i64> {
        let file = file.ok_or(EINVAL)?;
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };

        let base = u64::from(sqe.slot)
            .checked_mul(u64::from(cfg.slot_size))
            .ok_or(EINVAL)?;
        let mut pos = usize::try_from(base).map_err(|_| EINVAL)?;
        let mut fpos = sqe.off as i64;
        let len = sqe.len as usize;
        let mut done = 0usize;

        let mut bounce = KVec::with_capacity(PAGE_SIZE, GFP_KERNEL)?;
        bounce.resize(PAGE_SIZE, 0u8, GFP_KERNEL)?;

        while done < len {
            let in_page = pos % PAGE_SIZE;
            let want = core::cmp::min(len - done, PAGE_SIZE - in_page);

            {
                let arena = self.arena.lock();
                let page = arena.pages.get(pos / PAGE_SIZE).ok_or(EINVAL)?;
                // SAFETY: `bounce` is valid for `want` bytes and `in_page +
                // want <= PAGE_SIZE`. The slot is claimed for the life of this
                // op, so nothing else touches the page.
                unsafe { page.read_raw(bounce.as_mut_ptr(), in_page, want)? };
            }

            // The arena lock must NOT be held here, for the reason `do_read`
            // records: this reaches the VFS, which takes the inode rwsem.
            let n = match write_at(file, &bounce[..want], &mut fpos) {
                Ok(n) => n,
                Err(e) => return if done == 0 { Err(e) } else { Ok(done as i64) },
            };
            if n == 0 {
                break; // No progress: report what went out.
            }

            pos += n;
            done += n;
        }

        Ok(done as i64)
    }

    /// Copy `buf` into the arena at `pos`, splitting at page boundaries.
    ///
    /// Never call this with anything that can reach the VFS still to come: it
    /// holds the arena mutex, which `do_read` records the rule for.
    fn write_slot(&self, mut pos: usize, buf: &[u8]) -> Result<()> {
        let arena = self.arena.lock();
        let mut done = 0;

        while done < buf.len() {
            let page = arena.pages.get(pos / PAGE_SIZE).ok_or(EINVAL)?;
            let in_page = pos % PAGE_SIZE;
            let n = core::cmp::min(buf.len() - done, PAGE_SIZE - in_page);

            // SAFETY: `buf[done..]` is valid for `n` bytes and `in_page + n <=
            // PAGE_SIZE`. The slot is claimed for the life of this op, so
            // nothing else touches the page.
            unsafe { page.write_raw(buf[done..].as_ptr(), in_page, n)? };

            pos += n;
            done += n;
        }
        Ok(())
    }

    /// `STAT`: deferred, because `vfs_getattr` blocks on NFS and FUSE. Like
    /// `READ` it holds its slot, so it is in `held_slot`.
    fn stat_op(me: ArcBorrow<'_, RingCtx>, sqe: &Sqe) -> Option<i64> {
        let file = match RingCtx::stat_validate(&me, sqe) {
            Ok(file) => file,
            Err(e) => return Some(i64::from(e.to_errno())),
        };
        if !me.state.lock().slot_try_acquire(sqe.slot) {
            return Some(i64::from(EBUSY.to_errno()));
        }
        RingCtx::defer_holding_slot(me, sqe, Some(file))
    }

    /// Everything `STAT` can reject before it costs a work item.
    fn stat_validate(&self, sqe: &Sqe) -> Result<ARef<File>> {
        // `off` is a within-slot offset here, as on `OPEN` and `CHECKSUM`, so
        // `check_range` is the right check — unlike on `READ` and `WRITE`.
        self.check_range(sqe)?;
        // Every field is 64 bits. Refusing an unaligned destination is what
        // lets userspace read the struct in place instead of copying it out.
        if sqe.off % 8 != 0 {
            return Err(EINVAL);
        }
        // A stat that reports nothing can only be a caller bug, as an empty
        // `POLL_ADD` mask is.
        if sqe.len == 0 {
            return Err(EINVAL);
        }
        // No readability or file-type gate: a stat transfers no file data, and
        // `vfs_getattr` is defined on every type.
        self.handles.lock().resolve(sqe.handle)
    }

    /// Runs in a kworker. Returns the bytes written and the mask of fields the
    /// filesystem actually reported.
    fn do_stat(&self, sqe: &Sqe, file: Option<&File>, cred: &Credential) -> Result<(i64, u64)> {
        let file = file.ok_or(EINVAL)?;
        let pos = self.slot_offset(sqe)?;

        // SAFETY: the op holds an `ARef<File>`, so `f_path` is live; it sits in
        // an anonymous union bindgen names for us. The arena mutex is not held.
        let (out, mask) =
            unsafe { getattr(&raw const (*file.as_ptr()).__bindgen_anon_1.f_path, cred) }?;

        // `len` is the caller's buffer size and its version negotiation: an
        // older binary asks for less and gets exactly that much.
        let n = core::cmp::min(sqe.len as usize, core::mem::size_of::<KoruStat>());
        self.write_slot(pos, &out.as_bytes()[..n])?;
        Ok((n as i64, statx_to_koru(mask)))
    }

    /// The four path ops. `res` is 0, or a length for `READLINK` and
    /// `STATX_AT`; only `STATX_AT` sets `extra`.
    ///
    /// **Inline, permanently**: in a kworker `current_cred()` is `init_cred`
    /// and `current->fs` is the init root, and `override_creds` is not
    /// exported. doc/Notes.md's finding 3.
    fn path_op(&self, sqe: &Sqe) -> (i64, u64) {
        let failed = |e: Error| (i64::from(e.to_errno()), 0u64);
        match sqe.opcode {
            KORU_OP_TRUNCATE => self.do_truncate(sqe).map_or_else(failed, |()| (0, 0)),
            KORU_OP_UTIMES => self.do_utimes(sqe).map_or_else(failed, |()| (0, 0)),
            KORU_OP_STATX_AT => self.do_statx_at(sqe).unwrap_or_else(failed),
            _ => self.do_readlink(sqe).map_or_else(failed, |res| (res, 0)),
        }
    }

    /// `STATX_AT`: `STAT`'s answer for a path rather than a handle. The struct
    /// replaces the path, as `READLINK`'s target does.
    fn do_statx_at(&self, sqe: &Sqe) -> Result<(i64, u64)> {
        if sqe.handle != 0 {
            return Err(EINVAL);
        }
        let pos = self.path_validate(sqe)?;
        // Aligned for `STAT`'s reason: every field is 64 bits.
        if sqe.off % 8 != 0 {
            return Err(EINVAL);
        }
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };
        // The answer replaces the path, so the room is the rest of the slot.
        // The whole struct or nothing: a prefix would leave the caller reading
        // its own path bytes as fields.
        if u64::from(cfg.slot_size) - sqe.off < core::mem::size_of::<KoruStat>() as u64 {
            return Err(EINVAL);
        }

        // Held for the whole op, as `READLINK`'s is.
        if !self.state.lock().slot_try_acquire(sqe.slot) {
            return Err(EBUSY);
        }
        let done = self.statx_at_held(sqe, pos);
        self.state.lock().slot_release(sqe.slot);
        done
    }

    fn statx_at_held(&self, sqe: &Sqe, pos: usize) -> Result<(i64, u64)> {
        // Consumed before anything is written, so the overlap raises no
        // acquisition-order question.
        let path = self.copy_path(pos, sqe.len as usize)?;
        let cpath = CStr::from_bytes_with_nul(&path).map_err(|_| EINVAL)?;
        let p = Lookup::new(cpath, LOOKUP_FOLLOW)?;

        // Ioctl context, so `current` is the submitter and no carried cred is
        // needed: the ids translate in its namespace.
        let cred = current_cred();
        // SAFETY: `p` holds the path across the call, and the arena mutex is
        // not held here.
        let (out, mask) = unsafe { getattr(p.as_ptr(), &cred) }?;
        self.write_slot(pos, out.as_bytes())?;
        Ok((core::mem::size_of::<KoruStat>() as i64, statx_to_koru(mask)))
    }

    fn do_truncate(&self, sqe: &Sqe) -> Result<()> {
        let (path, arg) = self.path_and_arg(sqe, core::mem::size_of::<u64>())?;
        let length = u64::from_bytes_copy(&arg).ok_or(EINVAL)?;
        // `loff_t` is signed; a negative length is not a length. Same rule as
        // `READ` and `WRITE` apply to their file offsets.
        if length > i64::MAX as u64 {
            return Err(EINVAL);
        }
        let cpath = CStr::from_bytes_with_nul(&path).map_err(|_| EINVAL)?;
        let p = Lookup::new(cpath, LOOKUP_FOLLOW)?;

        // SAFETY: `p` holds the path across the call. No `mnt_want_write`:
        // `vfs_truncate` takes it, and permission-checks, itself.
        let ret = unsafe { bindings::vfs_truncate(p.as_ptr(), length as i64) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        Ok(())
    }

    fn do_utimes(&self, sqe: &Sqe) -> Result<()> {
        let (path, arg) = self.path_and_arg(sqe, core::mem::size_of::<KoruTimes>())?;
        let t = KoruTimes::from_bytes_copy(&arg).ok_or(EINVAL)?;

        // Pair by pair, never a transmute: `tv_nsec` is a `long`.
        let mut times = [
            bindings::timespec64 {
                tv_sec: t.atime_sec,
                tv_nsec: ffi::c_long::try_from(t.atime_nsec).map_err(|_| EINVAL)?,
            },
            bindings::timespec64 {
                tv_sec: t.mtime_sec,
                tv_nsec: ffi::c_long::try_from(t.mtime_nsec).map_err(|_| EINVAL)?,
            },
        ];

        let cpath = CStr::from_bytes_with_nul(&path).map_err(|_| EINVAL)?;
        let p = Lookup::new(cpath, LOOKUP_FOLLOW)?;

        // SAFETY: `p` holds the path and `times` outlives the call.
        // `vfs_utimes` validates the nanoseconds, sentinels included, and takes
        // `mnt_want_write` itself.
        let ret = unsafe { bindings::vfs_utimes(p.as_ptr(), times.as_mut_ptr()) };
        if ret < 0 {
            return Err(Error::from_errno(ret));
        }
        Ok(())
    }

    /// `READLINK`. Not `vfs_readlink`, whose buffer is a `char __user *`.
    fn do_readlink(&self, sqe: &Sqe) -> Result<i64> {
        if sqe.handle != 0 {
            return Err(EINVAL);
        }
        let pos = self.path_validate(sqe)?;
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };
        // The answer replaces the path, so the room is the rest of the slot.
        let room = (u64::from(cfg.slot_size) - sqe.off) as usize;

        // Held for the whole op, unlike `OPEN`'s: the answer goes back into
        // the slot the path came out of.
        if !self.state.lock().slot_try_acquire(sqe.slot) {
            return Err(EBUSY);
        }
        let done = self.readlink_held(sqe, pos, room);
        self.state.lock().slot_release(sqe.slot);
        done
    }

    fn readlink_held(&self, sqe: &Sqe, pos: usize, room: usize) -> Result<i64> {
        let path = self.copy_path(pos, sqe.len as usize)?;
        let cpath = CStr::from_bytes_with_nul(&path).map_err(|_| EINVAL)?;

        // No `LOOKUP_FOLLOW`: following the final symlink is what this op
        // must not do.
        let p = Lookup::new(cpath, 0)?;
        let link = Link::get(p.dentry())?;
        let target = link.bytes();

        // Truncating a path silently is how a wrong path gets used.
        if target.len() + 1 > room {
            return Err(enametoolong());
        }
        let mut out = KVec::with_capacity(target.len() + 1, GFP_KERNEL)?;
        out.extend_from_slice(target, GFP_KERNEL)?;
        out.push(0u8, GFP_KERNEL)?;
        self.write_slot(pos, &out)?;
        Ok(target.len() as i64)
    }

    /// `ADOPT_FD`: a handle for an already-open descriptor.
    ///
    /// Inline, for a different reason from `OPEN`: `fget` resolves against
    /// `current->files`, which in a kworker is the kthread's table.
    fn adopt_op(&self, ring: &File, sqe: &Sqe) -> i64 {
        match self.do_adopt(ring, sqe) {
            Ok(handle) => i64::from(handle),
            Err(e) => i64::from(e.to_errno()),
        }
    }

    fn do_adopt(&self, ring: &File, sqe: &Sqe) -> Result<u32> {
        // ADOPT_FD reads only `off`.
        if sqe.len != 0 || sqe.slot != 0 || sqe.handle != 0 {
            return Err(EINVAL);
        }
        // Bounded before `fget` sees it, so AT_FDCWD-style magic numbers never
        // reach it. O_PATH needs no check: `fget` is `__fget(fd, FMODE_PATH)`.
        if sqe.off > i32::MAX as u64 {
            return Err(EINVAL);
        }

        // No permission check, and none is owed: the task holds the descriptor
        // already, so this grants koru no authority it did not have.
        let local = LocalFile::fget(sqe.off as u32)?;
        // SAFETY: the safety condition holds because the ioctl path takes
        // `fdget`, not `fdget_pos`. Owning, not a light reference: this
        // outlives the ioctl and a kworker on another CPU will use it.
        let file = unsafe { LocalFile::assume_no_fdget_pos(local) };

        // Any koru fd, not just our own: two rings reach each other in two
        // hops. `f_op` is what they all share. Adopting one would put an
        // `Arc<RingCtx>` in a ring's own table, so `release` never runs.
        // SAFETY: both are live; `f_op` is set at open and never changes.
        let (theirs, ours) = unsafe { ((*file.as_ptr()).f_op, (*ring.as_ptr()).f_op) };
        if core::ptr::eq(theirs, ours) {
            return Err(eloop());
        }

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

    /// Queue an op that holds its slot, unwinding the claim if it never queues.
    fn defer_holding_slot(
        me: ArcBorrow<'_, RingCtx>,
        sqe: &Sqe,
        file: Option<ARef<File>>,
    ) -> Option<i64> {
        match RingCtx::defer(me, sqe, 0, file) {
            None => None,
            Some(res) => {
                me.state.lock().slot_release(sqe.slot);
                Some(res)
            }
        }
    }

    /// Allocate an op, count it in flight and register it for `CANCEL`. `Err`
    /// is the completion the failure owes, never an ioctl error (C1).
    /// Registration failure is fatal: an unregistered armed poll would outlive
    /// `release` with its entry on a freed file's queue.
    fn alloc_op(
        me: ArcBorrow<'_, RingCtx>,
        sqe: &Sqe,
        file: Option<ARef<File>>,
        interest: u32,
    ) -> core::result::Result<Arc<OpWork>, i64> {
        // Paired with `PinnedDrop for OpWork`. Taken before the allocation so
        // the drop impl always has a reference to release.
        module_get_live();

        // Taken for every deferred op rather than only for `STAT`: an op
        // resolves everything it needs at submit time, and a kworker has no way
        // back to this task.
        let cred = current_cred();

        let op = match Arc::pin_init(
            pin_init!(OpWork {
                work <- new_delayed_work!("OpWork::work"),
                poll <- PollState::new(interest),
                ring: Arc::from(me),
                sqe: *sqe,
                file: file,
                cred: cred,
            }),
            GFP_KERNEL,
        ) {
            Ok(op) => op,
            Err(e) => {
                module_put();
                return Err(i64::from(e.to_errno()));
            }
        };

        // Counted before enqueueing, or `run` could decrement first.
        me.state.lock().inflight += 1;

        // Registered before enqueueing: afterwards `run` may already have
        // completed and unregistered, and a late insert would never be removed.
        if me.pending.lock().push(op.clone(), GFP_KERNEL).is_err() {
            me.state.lock().inflight -= 1;
            return Err(i64::from(ENOMEM.to_errno()));
        }
        Ok(op)
    }

    /// Queue an op on the workqueue. `None` once it owns the reservation.
    fn defer(
        me: ArcBorrow<'_, RingCtx>,
        sqe: &Sqe,
        jiffies: Jiffies,
        file: Option<ARef<File>>,
    ) -> Option<i64> {
        let op = match RingCtx::alloc_op(me, sqe, file, 0) {
            Ok(op) => op,
            Err(res) => return Some(res),
        };

        match workqueue::system().enqueue_delayed(op, jiffies) {
            Ok(()) => None,
            // The queue handed the `Arc` back, so remove by identity: another
            // submitter may have pushed since.
            Err(op) => {
                RingCtx::pending_remove(&mut me.pending.lock(), &op);
                me.state.lock().inflight -= 1;
                Some(i64::from(EAGAIN.to_errno()))
            }
        }
    }

    /// `POLL_ADD`: arm a one-shot poll on `handle`.
    ///
    /// A third shape beside inline and deferred: *armed*. Nothing is queued;
    /// the op waits on the file's waitqueue and counts as in flight, so
    /// `ENTER` sleeps for it and `release` finds it.
    fn poll_op(me: ArcBorrow<'_, RingCtx>, sqe: &Sqe) -> Option<i64> {
        // POLL_ADD reads only `handle` and `len`.
        if sqe.off != 0 || sqe.slot != 0 {
            return Some(i64::from(EINVAL.to_errno()));
        }
        // An empty mask could only ever report an error, which is a bug in the
        // caller rather than a request.
        if sqe.len == 0 || sqe.len & !KORU_POLL_EVENTS_ALL != 0 {
            return Some(i64::from(EINVAL.to_errno()));
        }

        // No readability or blocking check: waiting is what a poll is for.
        let file = match me.handles.lock().resolve(sqe.handle) {
            Ok(file) => file,
            Err(e) => return Some(i64::from(e.to_errno())),
        };

        let interest = poll_to_kernel(sqe.len);
        let op = match RingCtx::alloc_op(me, sqe, Some(file), interest) {
            Ok(op) => op,
            Err(res) => return Some(res),
        };

        // The reference `run` consumes, leaked before the arm: the callback
        // can fire the instant the entry is queued, and from softirq it
        // cannot take one of its own.
        let credit = Arc::into_raw(op.clone());

        // SAFETY: ioctl context, and the op is not reachable from anywhere
        // else until the entry goes on the queue inside this call.
        let (mask, heads) = unsafe { poll_arm(&op, interest) };
        let ready = mask & (interest | POLL_ALWAYS);

        let res = if heads > 1 {
            i64::from(eopnotsupp().to_errno())
        } else if ready != 0 || heads == 0 {
            // Ready, or on no waitqueue at all — a file with no `poll` method,
            // which nothing can ever wake. Waiting on it would hold a CQ
            // reservation for the life of the ring, so answer now, even when
            // the answer is an empty mask.
            i64::from(poll_to_koru(ready))
        } else {
            // Armed. The wake callback, a `CANCEL` or `release` finishes it.
            return None;
        };

        // Ready, or refused — but the token still decides. A wake that beat us
        // has already queued the work, and completing twice would break C1.
        if !op.poll_claim() {
            return None;
        }
        // SAFETY: we won the token, so this is the one disarm, and a submit is
        // process context.
        unsafe { op.poll_disarm() };
        // SAFETY: we won, so `run` will never be called and nothing else will
        // ever reclaim the credit.
        drop(unsafe { Arc::from_raw(credit) });
        RingCtx::pending_remove(&mut me.pending.lock(), &op);
        me.state.lock().inflight -= 1;
        Some(res)
    }

    /// Cancel every queued op, for `release`.
    ///
    /// No completions are posted: the ring is being destroyed, and no ioctl can
    /// be in progress because the VFS holds a reference for the duration of
    /// one, so nothing is left to read a CQE or observe `inflight`. An op a
    /// worker already picked up cannot be cancelled and keeps the ring alive
    /// until it finishes, exactly as before.
    pub(crate) fn cancel_all(&self) {
        {
            let list = self.pending.lock();
            for op in list.iter() {
                // An armed poll is on no workqueue, so `cancel_delayed_work`
                // would answer false and leave the entry on a queue whose file
                // is about to be dropped. Disarm here, inside this lock and
                // before the registry goes: backwards is a use-after-free from
                // softirq, not a leak.
                let armed = op.sqe.opcode == KORU_OP_POLL_ADD && op.poll_claim();
                if armed {
                    // SAFETY: we won the token, and `release` is process context.
                    unsafe { op.poll_disarm() };
                }
                // SAFETY: the registry entry keeps the `OpWork` alive.
                if armed || unsafe { bindings::cancel_delayed_work(OpWork::delayed_work(op)) } {
                    // SAFETY: `run` will never reclaim what the arm or
                    // `enqueue_delayed` leaked, so adopt it. `op` still holds a
                    // reference, so this drop can never be the last.
                    drop(unsafe { Arc::from_raw(Arc::as_ptr(op)) });
                }
            }
        }
        // Outside the lock: this is where `fput` and `module_put` run.
        let drained = core::mem::replace(&mut *self.pending.lock(), KVec::new());
        drop(drained);
    }

    /// `CANCEL`: retire the in-flight op whose `user_data` is `off`.
    ///
    /// `cancel_delayed_work` returning true means it took the pending token from
    /// an armed timer or a worklist entry, so `run` will never be called — which
    /// is exactly the condition under which nobody else will reclaim the
    /// reference `enqueue_delayed` leaked. See doc/Notes.md; getting this wrong
    /// in either direction is a leak or a double free.
    fn cancel_op(&self, sqe: &Sqe) -> i64 {
        if sqe.len != 0 || sqe.slot != 0 || sqe.handle != 0 {
            return i64::from(EINVAL.to_errno());
        }

        let mut list = self.pending.lock();
        let Some(i) = RingCtx::pending_find(&list, sqe.off) else {
            return i64::from(ENOENT.to_errno());
        };
        // Cloned so it outlives the removal below and the unlock.
        let op = list[i].clone();

        // An armed poll is on no workqueue, so the token decides instead of
        // `cancel_delayed_work`. Losing it means the wake already queued the
        // op, which is `EALREADY` for the same reason a running op is.
        let armed = op.sqe.opcode == KORU_OP_POLL_ADD;
        let won = if armed {
            let won = op.poll_claim();
            if won {
                // SAFETY: we won the token, and this is process context.
                unsafe { op.poll_disarm() };
            }
            won
        } else {
            // SAFETY: `op` keeps the `OpWork`, and so its `delayed_work`, alive.
            unsafe { bindings::cancel_delayed_work(OpWork::delayed_work(&op)) }
        };
        if !won {
            // Already running, or already done. Nothing was pending, so no
            // reference is orphaned and none may be dropped.
            return i64::from(ealready().to_errno());
        }

        // SAFETY: the cancel succeeded, so `run` will never reclaim the
        // reference `enqueue_delayed` leaked. Adopt it here, exactly once.
        let leaked = unsafe { Arc::from_raw(Arc::as_ptr(&op)) };
        // In bounds: the index came from `pending_find` under this lock.
        let _ = list.remove(i);
        drop(list);
        drop(leaked);

        // C1: the target still gets its own completion, releasing whatever it
        // held. The ring `SpinLock` is taken only after the registry is unlocked.
        let target = op.sqe;
        self.complete(
            RingCtx::cqe(&target, i64::from(ecanceled().to_errno())),
            OpWork::held_slot(&target),
        );
        0
    }
}

// ---------------------------------------------------------------------------
// Stat
// ---------------------------------------------------------------------------

/// What `STAT` asks `vfs_getattr` for. `result_mask` says what came back.
const STAT_REQUEST_MASK: u32 = bindings::STATX_BASIC_STATS | bindings::STATX_BTIME;

/// `vfs_getattr` into a [`KoruStat`], plus the raw `result_mask`. Shared by
/// `STAT`, which carries the submitter's cred into a kworker, and `STATX_AT`,
/// which is the submitter.
///
/// # Safety
///
/// `path` must point at a live `struct path`, and the arena mutex must not be
/// held: this reaches the VFS.
unsafe fn getattr(path: *const bindings::path, cred: &Credential) -> Result<(KoruStat, u32)> {
    let mut ks = bindings::kstat::default();

    // SAFETY: the caller guarantees `path`, and `ks` is our own.
    let ret = unsafe {
        bindings::vfs_getattr(
            path,
            &mut ks,
            STAT_REQUEST_MASK,
            bindings::AT_STATX_SYNC_AS_STAT,
        )
    };
    if ret < 0 {
        return Err(Error::from_errno(ret));
    }

    // SAFETY: `cred` holds a reference, so its `user_ns` is live.
    let ns = unsafe { (*cred.as_ptr()).user_ns };
    // Every field is named, `reserved` explicitly zero, so a copy of this can
    // never show the caller its own stale bytes back as kernel values.
    let out = KoruStat {
        ino: ks.ino,
        size: ks.size as u64,
        blocks: ks.blocks,
        blksize: u64::from(ks.blksize),
        nlink: u64::from(ks.nlink),
        mode: u64::from(ks.mode),
        // Munged, as `stat(2)` is: an id with no mapping in `ns` reports
        // `overflowuid` rather than a raw `(uid_t)-1` nothing else uses.
        // SAFETY: `ns` is live and these are plain value translations.
        uid: u64::from(unsafe { bindings::from_kuid_munged(ns, ks.uid) }),
        // SAFETY: as above.
        gid: u64::from(unsafe { bindings::from_kgid_munged(ns, ks.gid) }),
        dev_major: u64::from(ks.dev >> MINORBITS),
        dev_minor: u64::from(ks.dev & MINORMASK),
        rdev_major: u64::from(ks.rdev >> MINORBITS),
        rdev_minor: u64::from(ks.rdev & MINORMASK),
        atime_sec: ks.atime.tv_sec,
        atime_nsec: ks.atime.tv_nsec as u64,
        mtime_sec: ks.mtime.tv_sec,
        mtime_nsec: ks.mtime.tv_nsec as u64,
        ctime_sec: ks.ctime.tv_sec,
        ctime_nsec: ks.ctime.tv_nsec as u64,
        btime_sec: ks.btime.tv_sec,
        btime_nsec: ks.btime.tv_nsec as u64,
        reserved: [0; 12],
    };
    Ok((out, ks.result_mask))
}

/// The submitting task's credentials. **Ioctl context only**: in a kworker
/// this is `init_cred`, which is why `OpWork` carries one.
fn current_cred() -> ARef<Credential> {
    // SAFETY: `current->cred` is replaced only by the task itself, so reading
    // it in that task's own ioctl needs no RCU.
    unsafe {
        let task = bindings::get_current();
        ARef::from(Credential::from_ptr((*task).cred))
    }
}

// `MAJOR` and `MINOR` are macros, so not in the bindings. From
// include/linux/kdev_t.h.
const MINORBITS: u32 = 20;
const MINORMASK: u32 = (1 << MINORBITS) - 1;

/// `kstat::result_mask` to koru's own bits, which are one per `KoruStat` field
/// and in that struct's order rather than statx's.
fn statx_to_koru(mask: u32) -> u64 {
    const PAIRS: [(u32, u64); 12] = [
        (bindings::STATX_INO, KORU_STAT_INO),
        (bindings::STATX_SIZE, KORU_STAT_SIZE),
        (bindings::STATX_BLOCKS, KORU_STAT_BLOCKS),
        (bindings::STATX_NLINK, KORU_STAT_NLINK),
        (bindings::STATX_TYPE, KORU_STAT_TYPE),
        (bindings::STATX_MODE, KORU_STAT_MODE),
        (bindings::STATX_UID, KORU_STAT_UID),
        (bindings::STATX_GID, KORU_STAT_GID),
        (bindings::STATX_ATIME, KORU_STAT_ATIME),
        (bindings::STATX_MTIME, KORU_STAT_MTIME),
        (bindings::STATX_CTIME, KORU_STAT_CTIME),
        (bindings::STATX_BTIME, KORU_STAT_BTIME),
    ];
    // No statx bit: `generic_fillattr` fills these from the inode every time.
    let mut out = KORU_STAT_BLKSIZE | KORU_STAT_DEV | KORU_STAT_RDEV;
    for (statx, koru) in PAIRS {
        if mask & statx != 0 {
            out |= koru;
        }
    }
    out
}

// Poll masks. Not in the bindings either: the `EPOLL*` constants carry the
// same `__force` cast. From include/uapi/asm-generic/poll.h, whose values the
// `EPOLL*` ones share.
const POLLIN: u32 = 0x0001;
const POLLPRI: u32 = 0x0002;
const POLLOUT: u32 = 0x0004;
const POLLERR: u32 = 0x0008;
const POLLHUP: u32 = 0x0010;
const POLLRDNORM: u32 = 0x0040;
const POLLWRNORM: u32 = 0x0100;
const POLLRDHUP: u32 = 0x2000;

/// What `vfs_poll` reports for a file with no `poll` method.
const DEFAULT_POLLMASK: u32 = POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM;
/// Reported whether or not they were asked for.
const POLL_ALWAYS: u32 = POLLERR | POLLHUP;

fn poll_to_kernel(events: u32) -> u32 {
    let mut mask = POLL_ALWAYS;
    if events & KORU_POLL_IN != 0 {
        mask |= POLLIN | POLLRDNORM;
    }
    if events & KORU_POLL_OUT != 0 {
        mask |= POLLOUT | POLLWRNORM;
    }
    if events & KORU_POLL_PRI != 0 {
        mask |= POLLPRI;
    }
    if events & KORU_POLL_RDHUP != 0 {
        mask |= POLLRDHUP;
    }
    mask
}

fn poll_to_koru(mask: u32) -> u32 {
    let mut events = 0;
    if mask & (POLLIN | POLLRDNORM) != 0 {
        events |= KORU_POLL_IN;
    }
    if mask & (POLLOUT | POLLWRNORM) != 0 {
        events |= KORU_POLL_OUT;
    }
    if mask & POLLPRI != 0 {
        events |= KORU_POLL_PRI;
    }
    if mask & POLLRDHUP != 0 {
        events |= KORU_POLL_RDHUP;
    }
    if mask & POLLERR != 0 {
        events |= KORU_POLL_ERR;
    }
    if mask & POLLHUP != 0 {
        events |= KORU_POLL_HUP;
    }
    events
}

/// `vfs_poll`, which is a static inline, with our own poll table.
///
/// # Safety
///
/// Process context: `add_wait_queue` takes the head's spinlock.
unsafe fn poll_arm(op: &Arc<OpWork>, interest: u32) -> (u32, u32) {
    let Some(file) = op.file.as_ref() else {
        return (0, 0);
    };
    let mut table = KoruPollTable {
        pt: bindings::poll_table_struct {
            _qproc: Some(poll_queue_proc),
            _key: interest,
        },
        op: Arc::as_ptr(op),
        heads: 0,
    };
    // SAFETY: a live file always has a valid `f_op`, and `poll` is called with
    // our own table, which outlives the call.
    unsafe {
        match (*(*file.as_ptr()).f_op).poll {
            None => (DEFAULT_POLLMASK, 0),
            Some(poll) => (poll(file.as_ptr(), &mut table.pt), table.heads),
        }
    }
}

/// The file's mask now, queueing nothing. For a keyless wake.
///
/// # Safety
///
/// Process context, and the file must still be alive.
unsafe fn poll_mask_now(op: &OpWork) -> u32 {
    let Some(file) = op.file.as_ref() else {
        return 0;
    };
    let mut table = bindings::poll_table_struct {
        _qproc: None,
        _key: !0,
    };
    // SAFETY: as `poll_arm`; a null `_qproc` is the ask-only form.
    unsafe {
        match (*(*file.as_ptr()).f_op).poll {
            None => DEFAULT_POLLMASK,
            Some(poll) => poll(file.as_ptr(), &mut table),
        }
    }
}

// Not in the bindings: bindgen cannot evaluate their `__force` casts. From
// include/linux/fs.h.
const FMODE_READ: u32 = 1 << 0;
const FMODE_WRITE: u32 = 1 << 1;
const FMODE_LSEEK: u32 = 1 << 2;
const FMODE_CAN_READ: u32 = 1 << 17;
const FMODE_CAN_WRITE: u32 = 1 << 18;

/// `i_mode & S_IFMT` for an open file.
fn file_type(file: &File) -> u32 {
    // SAFETY: an `ARef<File>` holds a reference, so the file and the inode it
    // pins are both live. `f_inode` is set at open and never changes.
    let i_mode = unsafe { (*(*file.as_ptr()).f_inode).i_mode };
    u32::from(i_mode) & bindings::S_IFMT
}

/// Reject a file `kernel_read` cannot read, before it warns about it.
///
/// `__kernel_read` answers all three of these with `-EINVAL` *and* a
/// `WARN_ON_ONCE` splat, so checking here is what keeps the log clean.
fn check_readable(file: &File) -> Result<()> {
    // SAFETY: as above. `f_mode` is immutable after open, so an unsynchronised
    // read is correct.
    let (f_mode, f_op) = unsafe { ((*file.as_ptr()).f_mode, (*file.as_ptr()).f_op) };

    if f_mode & (FMODE_READ | FMODE_CAN_READ) != FMODE_READ | FMODE_CAN_READ {
        return Err(EBADF);
    }
    // A blocking read in a kworker cannot be interrupted, so a FIFO or a socket
    // would wedge a worker for good unless it was opened non-blocking.
    if file_type(file) != bindings::S_IFREG && !is_nonblock(file) {
        return Err(EINVAL);
    }
    // SAFETY: a live file always has a valid `f_op`.
    let (read, read_iter) = unsafe { ((*f_op).read, (*f_op).read_iter) };
    if read.is_some() || read_iter.is_none() {
        return Err(EINVAL);
    }
    Ok(())
}

/// Reject a file `kernel_write` cannot write, before it warns about it.
///
/// Only the first guard is reachable today: `OPEN` refuses every non-regular
/// file, and a directory cannot be opened for writing. The other two are in
/// for when `ADOPT_FD` lands. See doc/Notes.md.
fn check_writable(file: &File) -> Result<()> {
    // SAFETY: as above. `f_mode` is immutable after open.
    let (f_mode, f_op) = unsafe { ((*file.as_ptr()).f_mode, (*file.as_ptr()).f_op) };

    if f_mode & (FMODE_WRITE | FMODE_CAN_WRITE) != FMODE_WRITE | FMODE_CAN_WRITE {
        return Err(EBADF);
    }
    // Same rule as READ: non-regular needs O_NONBLOCK, or a kworker wedges.
    if file_type(file) != bindings::S_IFREG && !is_nonblock(file) {
        return Err(EINVAL);
    }
    // SAFETY: a live file always has a valid `f_op`.
    let (write, write_iter) = unsafe { ((*f_op).write, (*f_op).write_iter) };
    if write.is_some() || write_iter.is_none() {
        return Err(EINVAL);
    }
    Ok(())
}

/// Whether this file was opened non-blocking. koru only ever reads this bit;
/// it never sets or clears it on a file it did not open.
fn is_nonblock(file: &File) -> bool {
    // SAFETY: as above.
    let f_flags = unsafe { (*file.as_ptr()).f_flags };
    f_flags & bindings::O_NONBLOCK != 0
}

/// Whether `off` means anything on this file.
fn is_seekable(file: &File) -> bool {
    // SAFETY: as above.
    let f_mode = unsafe { (*file.as_ptr()).f_mode };
    f_mode & FMODE_LSEEK != 0
}

/// One `kernel_write`, advancing `fpos`.
fn write_at(file: &File, buf: &[u8], fpos: &mut i64) -> Result<usize> {
    // SAFETY: `file` is live, `buf` is valid for reading its own length, and
    // `fpos` points at a live `loff_t`.
    let ret = unsafe {
        bindings::kernel_write(
            file.as_ptr(),
            buf.as_ptr().cast(),
            buf.len(),
            core::ptr::from_mut(fpos),
        )
    };
    if ret < 0 {
        return Err(Error::from_errno(ret as core::ffi::c_int));
    }
    Ok(ret as usize)
}

/// One `kernel_read`, advancing `fpos`. `Ok(0)` is EOF.
fn read_at(file: &File, buf: &mut [u8], fpos: &mut i64) -> Result<usize> {
    // SAFETY: `file` is live, `buf` is valid for writing its own length, and
    // `fpos` points at a live `loff_t`.
    let ret = unsafe {
        bindings::kernel_read(
            file.as_ptr(),
            buf.as_mut_ptr().cast(),
            buf.len(),
            core::ptr::from_mut(fpos),
        )
    };
    if ret < 0 {
        return Err(Error::from_errno(ret as core::ffi::c_int));
    }
    Ok(ret as usize)
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
    if flags & KORU_O_NONBLOCK != 0 {
        out |= bindings::O_NONBLOCK;
    }
    // Always: kernel opens of regular files, with no controlling terminal.
    out |= bindings::O_LARGEFILE | bindings::O_NOCTTY;
    i32::try_from(out).map_err(|_| EINVAL)
}
