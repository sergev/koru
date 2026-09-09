// SPDX-License-Identifier: GPL-2.0

//! xring: an experimental non-POSIX kernel API designed for coroutines.
//!
//! Submission and completion are separate events, because a coroutine must yield
//! to its executor between the call and the answer. See `Plan.md` in the
//! repository root for the design.
//!
//! T8: the control plane, the mmap'd arena, and kernel-enforced slot
//! exclusivity. Buffers are named by slot index and live in pages the kernel
//! owns, never by userspace address.

mod xring_abi;

use kernel::{
    bindings,
    fs::File,
    ioctl::{_IOC_NR, _IOC_SIZE, _IOC_TYPE},
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    mm::virt::{flags as vmflags, VmaNew},
    page::{Page, PAGE_SIZE},
    impl_has_delayed_work, new_condvar, new_delayed_work, new_mutex, new_spinlock,
    prelude::*,
    sync::{Arc, ArcBorrow, CondVar, CondVarTimeoutResult, Mutex, SpinLock},
    time::{msecs_to_jiffies, Jiffies},
    uaccess::{UserPtr, UserSlice, UserSliceReader, UserSliceWriter},
    workqueue::{self, DelayedWork, WorkItem},
};

use xring_abi::*;

module! {
    type: XringModule,
    name: "xring",
    authors: ["Serge Vakulenko"],
    description: "Coroutine-oriented async syscall interface",
    license: "GPL",
}

/// Module state. `MiscDeviceRegistration` deregisters in `Drop`.
#[pin_data(PinnedDrop)]
struct XringModule {
    #[pin]
    _miscdev: MiscDeviceRegistration<RingCtx>,
}

impl kernel::InPlaceModule for XringModule {
    fn init(_module: &'static ThisModule) -> impl PinInit<Self, Error> {
        // `pr_info!` already prefixes the module name, so do not repeat it.
        pr_info!("init, registering /dev/xring\n");

        // No mode field, so the node is created 0600 root:root, as intended.
        let options = MiscDeviceOptions { name: c"xring" };

        try_pin_init!(Self {
            _miscdev <- MiscDeviceRegistration::register(options),
        })
    }
}

// `#[pin_data]` generates its own `Drop`, so the hook goes through `PinnedDrop`.
#[pinned_drop]
impl PinnedDrop for XringModule {
    fn drop(self: Pin<&mut Self>) {
        pr_info!("exit\n");
    }
}

/// Nanoseconds to jiffies, rounded up so a sub-millisecond delay still waits.
fn delay_jiffies(ns: u64) -> Jiffies {
    msecs_to_jiffies(ns.div_ceil(1_000_000).try_into().unwrap_or(u32::MAX))
}

/// Pin the module. `false` means it is already going away.
///
/// The Rust `miscdevice` abstraction builds its `file_operations` with
/// `..zeroed()`, so `fops.owner` is NULL and `fops_get` pins nothing. Without
/// this, `rmmod` frees text that an open file still points into.
fn module_get() -> bool {
    // SAFETY: `THIS_MODULE` is valid for the life of the module.
    unsafe { bindings::try_module_get(THIS_MODULE.as_ptr()) }
}

/// Pin the module when a reference is already held, so it cannot fail.
fn module_get_live() {
    // SAFETY: the caller holds a reference via the open fd, so the refcount is
    // non-zero and this cannot race the module going away.
    unsafe { bindings::__module_get(THIS_MODULE.as_ptr()) }
}

/// Drop a reference taken by [`module_get`] or [`module_get_live`].
///
/// Called from module text, so a *blocking* `delete_module` could in principle
/// proceed while this function returns. Default `rmmod` is non-blocking and
/// fails on a non-zero count, so the window needs a blocking unload racing the
/// last close. The real fix is upstream: `MiscDeviceOptions` has no way to set
/// `fops.owner`, which is what makes the VFS drop the reference from core text.
fn module_put() {
    // SAFETY: balanced against a get taken on this path.
    unsafe { bindings::module_put(THIS_MODULE.as_ptr()) }
}

/// What `SETUP` established. Kept separately from the wire struct so the kernel
/// never reads back values it copied from userspace.
#[derive(Copy, Clone)]
struct RingConfig {
    sq_entries: u32,
    cq_entries: u32,
    slot_size: u32,
    slot_count: u32,
    arena_size: u64,
}

/// Fixed-capacity completion FIFO, allocated at `SETUP` so `ENTER` never
/// allocates. Used circularly: `cq.len()` is capacity, not occupancy.
///
/// `reserved` counts slots claimed by a consumed SQE that has not completed yet;
/// `inflight` counts deferred ops among them. Reserving before consuming is what
/// makes CQ overflow unrepresentable.
struct RingState {
    cq: KVec<Cqe>,
    head: usize,
    len: usize,
    reserved: usize,
    inflight: usize,
    /// One bit per arena slot, set while an op owns it.
    ///
    /// `index < slot_count` is not sufficient validation: nothing stops
    /// userspace naming one slot in two concurrent SQEs, which would race two
    /// accesses on one page and break `read_raw`'s no-concurrent-access
    /// precondition. Exclusivity is enforced here; the userspace pool is
    /// advisory.
    slot_busy: KVec<u64>,
}

impl RingState {
    fn capacity(&self) -> usize {
        self.cq.len()
    }

    /// Claim a slot. False means full: stop consuming, report a short count.
    fn reserve(&mut self) -> bool {
        if self.len + self.reserved >= self.capacity() {
            return false;
        }
        self.reserved += 1;
        true
    }

    /// Return an unused claim.
    fn unreserve(&mut self) {
        self.reserved -= 1;
    }

    /// Post a completion, consuming its reservation.
    fn post(&mut self, cqe: Cqe) {
        let cap = self.capacity();
        let idx = (self.head + self.len) % cap;
        self.cq[idx] = cqe;
        self.len += 1;
        self.reserved -= 1;
    }

    fn front(&self) -> Option<Cqe> {
        (self.len > 0).then(|| self.cq[self.head])
    }

    fn pop(&mut self) {
        self.head = (self.head + 1) % self.capacity();
        self.len -= 1;
    }

    /// Nothing queued and nothing that could ever arrive.
    fn idle(&self) -> bool {
        self.len == 0 && self.inflight == 0
    }

    /// Claim a slot. False means another op already owns it.
    fn slot_try_acquire(&mut self, slot: u32) -> bool {
        let (w, b) = (slot as usize / 64, slot as usize % 64);
        if self.slot_busy[w] & (1u64 << b) != 0 {
            return false;
        }
        self.slot_busy[w] |= 1u64 << b;
        true
    }

    fn slot_release(&mut self, slot: u32) {
        let (w, b) = (slot as usize / 64, slot as usize % 64);
        self.slot_busy[w] &= !(1u64 << b);
    }
}

/// Per-fd ring context, root of the object graph. Plan.md's Lifetimes section
/// lists what lands here next.
#[pin_data]
struct RingCtx {
    /// `Mutex`, not `SpinLock`: nothing on this path is atomic context.
    #[pin]
    config: Mutex<Option<RingConfig>>,

    /// Serialises all of `ENTER`: the consumer must be single-threaded, and it
    /// keeps a second reaper out of the peek-then-pop window.
    ///
    /// T5 must drop this around the `CondVar` wait, or a waiter blocks every
    /// submitter on the ring.
    #[pin]
    submit_lock: Mutex<()>,

    /// `SpinLock`: from T5 workers post completions from non-sleepable context.
    ///
    /// **No `UserSlice` access while held** — `copy_*_user` can fault and sleep.
    #[pin]
    state: SpinLock<RingState>,

    /// Waits on `state`. Notified by every completion, inline or deferred.
    #[pin]
    cq_wait: CondVar,

    /// The arena: one entry per page, allocated at `SETUP`.
    ///
    /// `vm_insert_page` takes its own reference, so these can be dropped while a
    /// mapping still exists. That is what makes `close(fd)` before `munmap`
    /// safe. Guarded by `config`'s lock, which is only taken outside atomic
    /// context.
    #[pin]
    arena: Mutex<Arena>,
}

/// Kernel-owned buffer pages, plus whether they have been mapped.
struct Arena {
    pages: KVec<Page>,
    /// `mmap` is one-shot: a second call gets `EBUSY`.
    mapped: bool,
}

/// A deferred operation. Plan.md's Lifetimes diagram is the shape.
///
/// Holds its own `Arc<RingCtx>`, so the ring outlives `close(fd)` while work is
/// still queued and the last one out frees it.
///
/// Also holds a module reference for its whole life, including the delay before
/// it runs: a queued work item points at `run` in module text, and nothing else
/// keeps that text alive once the fd is closed.
#[pin_data(PinnedDrop)]
struct OpWork {
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
            XRING_OP_DELAY_NS => (0, None),
            XRING_OP_CHECKSUM => (
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

#[vtable]
impl MiscDevice for RingCtx {
    type Ptr = Arc<Self>;

    fn open(_file: &File, _misc: &MiscDeviceRegistration<Self>) -> Result<Arc<Self>> {
        if !module_get() {
            return Err(ENODEV);
        }
        let ctx = Arc::pin_init(
            pin_init!(RingCtx {
                config <- new_mutex!(None),
                submit_lock <- new_mutex!(()),
                // Zero capacity until `SETUP` installs the real queue.
                state <- new_spinlock!(RingState {
                    cq: KVec::new(),
                    head: 0,
                    len: 0,
                    reserved: 0,
                    inflight: 0,
                    slot_busy: KVec::new(),
                }),
                cq_wait <- new_condvar!("RingCtx::cq_wait"),
                arena <- new_mutex!(Arena {
                    pages: KVec::new(),
                    mapped: false,
                }),
            }),
            GFP_KERNEL,
        );
        if ctx.is_err() {
            module_put();
        }
        ctx
    }

    /// Map the arena. One-shot, exact length, `MAP_SHARED` only.
    fn mmap(me: ArcBorrow<'_, RingCtx>, _file: &File, vma: &VmaNew) -> Result {
        let Some(cfg) = *me.config.lock() else {
            return Err(EINVAL);
        };

        // No `vm_pgoff` accessor on `VmaNew`, so read it raw. A non-zero offset
        // would mean mapping part of the arena, which we do not support.
        // SAFETY: the VMA is valid for this call and undergoing setup.
        let pgoff = unsafe { (*vma.as_ptr()).vm_pgoff };
        if pgoff != 0 {
            return Err(EINVAL);
        }

        // Exact, not "at least": a short mapping would leave slots unbacked.
        let len = vma.end() - vma.start();
        if len as u64 != cfg.arena_size {
            return Err(EINVAL);
        }

        // MAP_PRIVATE would silently give copy-on-write, so userspace writes
        // would land in a private copy the kernel never sees. Plan.md calls
        // this the single most likely day-loser.
        if vma.flags() & vmflags::SHARED == 0 {
            return Err(EINVAL);
        }

        let mut arena = me.arena.lock();
        if arena.mapped {
            return Err(EBUSY);
        }

        // DONTCOPY keeps the arena out of forked children. MIXEDMAP and
        // DONTEXPAND are both in VM_SPECIAL, which is what makes MADV_DOFORK
        // refuse, so userspace cannot undo it. VM_IO is not needed for that and
        // is not set.
        let mm = vma.set_mixedmap();
        vma.set_dontcopy();
        vma.set_dontexpand();

        for (i, page) in arena.pages.iter().enumerate() {
            // Propagate a partial insert: the kernel tears down the VMA on a
            // failed mmap, which beats a half-backed arena that reads as valid.
            mm.vm_insert_page(vma.start() + i * PAGE_SIZE, page)?;
        }

        arena.mapped = true;
        Ok(())
    }

    /// Drops the `Arc`, then the module reference `open` took.
    fn release(device: Arc<Self>, _file: &File) {
        drop(device);
        module_put();
    }

    fn ioctl(me: ArcBorrow<'_, RingCtx>, _file: &File, cmd: u32, arg: usize) -> Result<isize> {
        // Dispatch on type and command number: matching the whole ioctl number
        // would report a version skew as "no such ioctl" and hide it.
        if _IOC_TYPE(cmd) != XRING_IOC_TYPE {
            return Err(ENOTTY);
        }

        let ptr = UserPtr::from_addr(arg);

        // Only for a recognised command: a mismatch means ABI skew.
        let check_size = |expected: usize| {
            if _IOC_SIZE(cmd) == expected {
                Ok(())
            } else {
                Err(eproto())
            }
        };
        let params_size = core::mem::size_of::<XringParams>();
        let enter_size = core::mem::size_of::<XringEnter>();

        match _IOC_NR(cmd) {
            XRING_NR_SETUP => {
                check_size(params_size)?;
                let (reader, writer) = UserSlice::new(ptr, params_size).reader_writer();
                me.setup(reader, writer)?;
                Ok(0)
            }
            XRING_NR_GET_PARAMS => {
                check_size(params_size)?;
                me.get_params(UserSlice::new(ptr, params_size).writer())?;
                Ok(0)
            }
            // The only ioctl returning a value: SQEs consumed (E1).
            XRING_NR_ENTER => {
                check_size(enter_size)?;
                RingCtx::enter(me, ptr)
            }
            _ => Err(ENOTTY),
        }
    }

}

impl RingCtx {
    /// Fill what the kernel always owns: caps and ABI identity.
    fn fill_caps(p: &mut XringParams) {
        p.magic = XRING_MAGIC;
        p.abi_version = XRING_ABI_VERSION;
        p.features = 0;
        p.max_sq_entries = XRING_MAX_SQ_ENTRIES;
        p.max_cq_entries = XRING_MAX_CQ_ENTRIES;
        p.max_slot_size = XRING_MAX_SLOT_SIZE;
        p.max_slot_count = XRING_MAX_SLOT_COUNT;
        p.max_arena_bytes = XRING_MAX_ARENA_BYTES;
    }

    /// Validate a `SETUP` request. Pure, so a rejected one cannot consume the
    /// one-shot.
    fn validate(req: &XringParams) -> Result<RingConfig> {
        // Identity first: ABI skew should not surface as a field complaint.
        if req.magic != XRING_MAGIC || req.abi_version != XRING_ABI_VERSION {
            return Err(eproto());
        }

        // Reserved-zero and unknown-flag rejection: the extensibility rule.
        if req.flags & !XRING_SETUP_FLAGS_ALL != 0 {
            return Err(EINVAL);
        }
        if req.reserved.iter().any(|&r| r != 0) {
            return Err(EINVAL);
        }

        let sq_entries = req.sq_entries;
        if sq_entries == 0 || sq_entries > XRING_MAX_SQ_ENTRIES {
            return Err(EINVAL);
        }

        // 0 means "same as sq_entries".
        let cq_entries = if req.cq_entries == 0 {
            sq_entries
        } else {
            req.cq_entries
        };
        if cq_entries > XRING_MAX_CQ_ENTRIES {
            return Err(EINVAL);
        }
        // A shallower CQ would break the reserve-per-SQE rule.
        if cq_entries < sq_entries {
            return Err(EINVAL);
        }

        let slot_size = req.slot_size;
        let slot_count = req.slot_count;
        if slot_size == 0 || slot_size > XRING_MAX_SLOT_SIZE {
            return Err(EINVAL);
        }
        // Whole pages per slot: the arena is order-0 pages, and a slot that
        // straddled a page boundary would put split logic on the security
        // boundary. Also makes `arena_size` exactly page-aligned.
        if slot_size as usize % PAGE_SIZE != 0 {
            return Err(EINVAL);
        }
        if slot_count == 0 || slot_count > XRING_MAX_SLOT_COUNT {
            return Err(EINVAL);
        }

        // User-controlled u32s: this product overflows trivially.
        let arena_size = u64::from(slot_size)
            .checked_mul(u64::from(slot_count))
            .ok_or(EINVAL)?;
        if arena_size > XRING_MAX_ARENA_BYTES {
            return Err(EINVAL);
        }

        Ok(RingConfig {
            sq_entries,
            cq_entries,
            slot_size,
            slot_count,
            arena_size,
        })
    }

    /// `XRING_IOC_SETUP`: configure the ring exactly once.
    fn setup(&self, mut reader: UserSliceReader, mut writer: UserSliceWriter) -> Result {
        let req: XringParams = reader.read()?;
        let cfg = Self::validate(&req)?;

        // Allocate before locking, so ENTER never allocates.
        let n = cfg.cq_entries as usize;
        let mut cq = KVec::with_capacity(n, GFP_KERNEL)?;
        for _ in 0..n {
            cq.push(Cqe::default(), GFP_KERNEL)?;
        }

        // The arena too: SETUP reports `arena_size`, so it should fail here
        // rather than promise memory `mmap` cannot deliver.
        let npages = (cfg.arena_size as usize) / PAGE_SIZE;
        let mut pages = KVec::with_capacity(npages, GFP_KERNEL)?;
        for _ in 0..npages {
            let page = Page::alloc_page(GFP_KERNEL)?;
            // Never hand userspace whatever was in the page before.
            // SAFETY: we hold the only reference; nothing else can touch it.
            unsafe { page.fill_zero_raw(0, PAGE_SIZE)? };
            pages.push(page, GFP_KERNEL)?;
        }

        let nwords = (cfg.slot_count as usize).div_ceil(64);
        let mut slot_busy = KVec::with_capacity(nwords, GFP_KERNEL)?;
        for _ in 0..nwords {
            slot_busy.push(0u64, GFP_KERNEL)?;
        }

        {
            let mut guard = self.config.lock();
            if guard.is_some() {
                return Err(EBUSY);
            }
            // Under the config lock, so no ENTER sees configured-but-empty.
            let mut state = self.state.lock();
            state.cq = cq;
            state.head = 0;
            state.len = 0;
            state.reserved = 0;
            state.inflight = 0;
            state.slot_busy = slot_busy;
            drop(state);

            let mut arena = self.arena.lock();
            arena.pages = pages;
            arena.mapped = false;
            drop(arena);

            *guard = Some(cfg);
        }

        // Built from scratch, not echoed: no unvalidated bytes travel back.
        let mut out = XringParams::default();
        Self::fill_caps(&mut out);
        Self::fill_config(&mut out, &cfg);
        writer.write(&out)
    }

    /// `XRING_IOC_GET_PARAMS`: report caps always, effective values if configured.
    fn get_params(&self, mut writer: UserSliceWriter) -> Result {
        let mut out = XringParams::default();
        Self::fill_caps(&mut out);
        if let Some(cfg) = *self.config.lock() {
            Self::fill_config(&mut out, &cfg);
        }
        writer.write(&out)
    }

    /// `XRING_IOC_ENTER`: submit, then reap. Returns SQEs **consumed** (E1);
    /// completions are counted in `completed`.
    fn enter(me: ArcBorrow<'_, RingCtx>, arg: UserPtr) -> Result<isize> {
        let this = &*me;
        let size = core::mem::size_of::<XringEnter>();
        let (mut arg_reader, mut arg_writer) = UserSlice::new(arg, size).reader_writer();
        let mut req: XringEnter = arg_reader.read()?;

        // Protocol failures fail the ioctl; only SQE errors become completions.
        if req.flags & !XRING_ENTER_FLAGS_ALL != 0 {
            return Err(EINVAL);
        }
        if req.reserved.iter().any(|&r| r != 0) {
            return Err(EINVAL);
        }

        let Some(cfg) = *this.config.lock() else {
            // No SETUP, so no queue to submit to.
            return Err(EINVAL);
        };
        if req.to_submit > cfg.sq_entries {
            return Err(EINVAL);
        }
        // Unsatisfiable by construction.
        if req.min_complete > req.cq_space {
            return Err(EINVAL);
        }

        // User-controlled u32s: size the regions in u64.
        let entry = core::mem::size_of::<Sqe>() as u64;
        let sq_bytes = u64::from(req.to_submit).checked_mul(entry).ok_or(EINVAL)?;
        let cq_bytes = u64::from(req.cq_space)
            .checked_mul(core::mem::size_of::<Cqe>() as u64)
            .ok_or(EINVAL)?;
        let sq_bytes = usize::try_from(sq_bytes).map_err(|_| EINVAL)?;
        let cq_bytes = usize::try_from(cq_bytes).map_err(|_| EINVAL)?;

        // One submitter and one reaper at a time.
        let submitting = this.submit_lock.lock();

        let consumed = RingCtx::submit(
            me,
            UserSlice::new(UserPtr::from_addr(req.sq_addr as usize), sq_bytes).reader(),
            req.to_submit,
        )?;

        // Dropped before waiting: sleeping while holding it would block every
        // other submitter on the ring. Retaken for the reap.
        drop(submitting);
        let interrupted = this.wait(req.min_complete as usize, req.timeout_ns);
        let _submitting = this.submit_lock.lock();

        let completed = this.reap(
            UserSlice::new(UserPtr::from_addr(req.cq_addr as usize), cq_bytes).writer(),
            req.cq_space,
        );

        // Written even on the EINTR path, so the caller knows what not to
        // resubmit. A single ioctl return value cannot carry both.
        req.submitted = consumed;
        req.completed = completed;
        arg_writer.write(&req)?;

        if interrupted {
            return Err(EINTR);
        }
        Ok(consumed as isize)
    }

    /// Consume up to `to_submit` SQEs, returning how many. Every consumed SQE
    /// posts exactly one completion, malformed ones included (C1).
    fn submit(me: ArcBorrow<'_, RingCtx>, mut sq: UserSliceReader, to_submit: u32) -> Result<u32> {
        let this = &*me;
        let mut consumed: u32 = 0;

        for _ in 0..to_submit {
            // Reserve before consuming, so the completion always has a home.
            if !this.state.lock().reserve() {
                break; // Ring full: short submit count, not an error.
            }

            // Outside the spinlock: this can fault, and faulting sleeps.
            let sqe: Sqe = match sq.read() {
                Ok(sqe) => sqe,
                Err(e) => {
                    this.state.lock().unreserve();
                    // A faulting SQ array is not a completion. Report it only
                    // if nothing was consumed; else the short count says where.
                    return if consumed == 0 { Err(e) } else { Ok(consumed) };
                }
            };

            consumed += 1;
            match RingCtx::dispatch(me, &sqe) {
                // Completed inline: post now, consuming the reservation.
                Some(res) => this.state.lock().post(Self::cqe(&sqe, res)),
                // Deferred: the reservation stays claimed until `run` posts.
                None => {}
            }
        }

        Ok(consumed)
    }

    /// Post a completion and wake anyone waiting. Called from kworkers too.
    ///
    /// The slot is released in the same critical section that posts the CQE, so
    /// it is free exactly when userspace can see the completion, never before.
    fn complete(&self, cqe: Cqe, free_slot: Option<u32>) {
        {
            let mut state = self.state.lock();
            state.inflight -= 1;
            if let Some(slot) = free_slot {
                state.slot_release(slot);
            }
            state.post(cqe);
        }
        self.cq_wait.notify_all();
    }

    /// Wait for `min_complete` completions. Returns true if a signal arrived.
    fn wait(&self, min_complete: usize, timeout_ns: u64) -> bool {
        if min_complete == 0 {
            return false;
        }

        // `timeout_ns == 0` means no cap. Round up so a sub-millisecond request
        // does not become a zero-jiffy no-wait.
        let jiffies: Jiffies = if timeout_ns == 0 {
            Jiffies::MAX
        } else {
            msecs_to_jiffies(timeout_ns.div_ceil(1_000_000).try_into().unwrap_or(u32::MAX))
        };

        let mut state = self.state.lock();
        loop {
            if state.len >= min_complete {
                return false;
            }
            // Nothing queued and nothing that could ever arrive: returning
            // beats sleeping forever. io_uring gets this wrong.
            if state.idle() {
                return false;
            }
            match self.cq_wait.wait_interruptible_timeout(&mut state, jiffies) {
                CondVarTimeoutResult::Woken { .. } => {}
                CondVarTimeoutResult::Timeout => return false,
                CondVarTimeoutResult::Signal { .. } => return true,
            }
        }
    }

    /// Copy out up to `cq_space` completions. Returns how many were written.
    fn reap(&self, mut cq: UserSliceWriter, cq_space: u32) -> u32 {
        let mut completed: u32 = 0;

        while completed < cq_space {
            // Peek, copy, pop. Popping first would lose the completion on a
            // faulting copy, breaking C1. Posters only append at the tail.
            let Some(cqe) = self.state.lock().front() else {
                break;
            };
            if cq.write(&cqe).is_err() {
                break; // Leave it queued for the next ENTER.
            }
            self.state.lock().pop();
            completed += 1;
        }

        completed
    }

    fn cqe(sqe: &Sqe, res: i64) -> Cqe {
        Cqe {
            user_data: sqe.user_data,
            res,
            flags: 0,
            rsvd0: 0,
            extra: 0,
        }
    }

    /// Opcode dispatch. `Some(res)` completed inline, `None` was deferred and
    /// will post its own completion. Never fails the ioctl: per E1 a bad SQE is
    /// a completion.
    fn dispatch(me: ArcBorrow<'_, RingCtx>, sqe: &Sqe) -> Option<i64> {
        let einval = i64::from(EINVAL.to_errno());

        if sqe.rsvd0 != 0 || sqe.flags & !XRING_SQE_FLAGS_ALL != 0 {
            return Some(einval);
        }

        match sqe.opcode {
            XRING_OP_NOP => {
                // NOP reads no argument fields, so all must be zero.
                if sqe.len != 0 || sqe.off != 0 || sqe.slot != 0 || sqe.handle != 0 {
                    return Some(einval);
                }
                Some(0)
            }
            XRING_OP_DELAY_NS => {
                // DELAY_NS reads only `off`.
                if sqe.len != 0 || sqe.slot != 0 || sqe.handle != 0 {
                    return Some(einval);
                }
                RingCtx::defer(me, sqe, delay_jiffies(sqe.off))
            }
            XRING_OP_CHECKSUM => {
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

    /// FNV-1a over `len` bytes at `off` in slot `slot`.
    ///
    /// Masked to 63 bits so the result is never mistaken for an errno.
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

    fn checksum(&self, sqe: &Sqe) -> Result<i64> {
        self.check_range(sqe)?;
        let Some(cfg) = *self.config.lock() else {
            return Err(EINVAL);
        };

        let base = u64::from(sqe.slot)
            .checked_mul(u64::from(cfg.slot_size))
            .ok_or(EINVAL)?
            .checked_add(sqe.off)
            .ok_or(EINVAL)?;
        let mut pos = usize::try_from(base).map_err(|_| EINVAL)?;
        let mut left = sqe.len as usize;

        let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
        let mut buf = [0u8; 256];
        let arena = self.arena.lock();

        while left > 0 {
            let page = arena.pages.get(pos / PAGE_SIZE).ok_or(EINVAL)?;
            let in_page = pos % PAGE_SIZE;
            let n = core::cmp::min(core::cmp::min(left, PAGE_SIZE - in_page), buf.len());

            // SAFETY: `buf` is valid for `n` bytes and `in_page + n <= PAGE_SIZE`.
            // Slot exclusivity, which is what rules out a concurrent writer, is
            // T8; until then nothing else touches a slot under test.
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

    fn fill_config(p: &mut XringParams, cfg: &RingConfig) {
        p.sq_entries = cfg.sq_entries;
        p.cq_entries = cfg.cq_entries;
        p.slot_size = cfg.slot_size;
        p.slot_count = cfg.slot_count;
        p.arena_size = cfg.arena_size;
        p.configured = 1;
    }
}
