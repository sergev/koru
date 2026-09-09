// SPDX-License-Identifier: GPL-2.0

//! xring: an experimental non-POSIX kernel API designed for coroutines.
//!
//! Submission and completion are separate events, because a coroutine must yield
//! to its executor between the call and the answer. See `Plan.md` in the
//! repository root for the design.
//!
//! T4: `SETUP`, `GET_PARAMS` and `ENTER` with `NOP`. No arena yet.

mod xring_abi;

use kernel::{
    fs::File,
    ioctl::{_IOC_NR, _IOC_SIZE, _IOC_TYPE},
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    new_mutex, new_spinlock,
    prelude::*,
    sync::{Arc, ArcBorrow, Mutex, SpinLock},
    uaccess::{UserPtr, UserSlice, UserSliceReader, UserSliceWriter},
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
/// `reserved` counts slots claimed by a consumed SQE that has not completed yet.
/// Reserving before consuming is what makes CQ overflow unrepresentable.
struct RingState {
    cq: KVec<Cqe>,
    head: usize,
    len: usize,
    reserved: usize,
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
}

#[vtable]
impl MiscDevice for RingCtx {
    type Ptr = Arc<Self>;

    fn open(_file: &File, _misc: &MiscDeviceRegistration<Self>) -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(RingCtx {
                config <- new_mutex!(None),
                submit_lock <- new_mutex!(()),
                // Zero capacity until `SETUP` installs the real queue.
                state <- new_spinlock!(RingState {
                    cq: KVec::new(),
                    head: 0,
                    len: 0,
                    reserved: 0,
                }),
            }),
            GFP_KERNEL,
        )
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
                me.enter(ptr)
            }
            _ => Err(ENOTTY),
        }
    }

    // `release` stays the default; `ForeignOwnable` balances the refcount.
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
            drop(state);
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
    fn enter(&self, arg: UserPtr) -> Result<isize> {
        let size = core::mem::size_of::<XringEnter>();
        let (mut arg_reader, mut arg_writer) = UserSlice::new(arg, size).reader_writer();
        let mut req: XringEnter = arg_reader.read()?;

        // Protocol failures fail the ioctl; only SQE errors become completions.
        if req.flags & !XRING_ENTER_FLAGS_ALL != 0 {
            return Err(EINVAL);
        }
        if req.rsvd0 != 0 || req.reserved.iter().any(|&r| r != 0) {
            return Err(EINVAL);
        }

        let Some(cfg) = *self.config.lock() else {
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
        let _submitting = self.submit_lock.lock();

        let consumed = self.submit(
            UserSlice::new(UserPtr::from_addr(req.sq_addr as usize), sq_bytes).reader(),
            req.to_submit,
        )?;

        // T5 adds the `CondVar` wait here for `min_complete`/`timeout_ns`.
        // Until then every opcode completes inline, so there is no wait.
        let completed = self.reap(
            UserSlice::new(UserPtr::from_addr(req.cq_addr as usize), cq_bytes).writer(),
            req.cq_space,
        );

        req.completed = completed;
        arg_writer.write(&req)?;
        Ok(consumed as isize)
    }

    /// Consume up to `to_submit` SQEs, returning how many. Every consumed SQE
    /// posts exactly one completion, malformed ones included (C1).
    fn submit(&self, mut sq: UserSliceReader, to_submit: u32) -> Result<u32> {
        let mut consumed: u32 = 0;

        for _ in 0..to_submit {
            // Reserve before consuming, so the completion always has a home.
            if !self.state.lock().reserve() {
                break; // Ring full: short submit count, not an error.
            }

            // Outside the spinlock: this can fault, and faulting sleeps.
            let sqe: Sqe = match sq.read() {
                Ok(sqe) => sqe,
                Err(e) => {
                    self.state.lock().unreserve();
                    // A faulting SQ array is not a completion. Report it only
                    // if nothing was consumed; else the short count says where.
                    return if consumed == 0 { Err(e) } else { Ok(consumed) };
                }
            };

            consumed += 1;
            let cqe = Self::execute(&sqe);
            self.state.lock().post(cqe);
        }

        Ok(consumed)
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

    /// Run one SQE and build the completion it owes.
    fn execute(sqe: &Sqe) -> Cqe {
        Cqe {
            user_data: sqe.user_data,
            res: Self::run(sqe),
            flags: 0,
            rsvd0: 0,
            extra: 0,
        }
    }

    /// Opcode dispatch. Returns the CQE `res`. Never fails the ioctl: per E1 a
    /// bad SQE is a completion.
    fn run(sqe: &Sqe) -> i64 {
        let einval = i64::from(EINVAL.to_errno());

        if sqe.rsvd0 != 0 || sqe.flags & !XRING_SQE_FLAGS_ALL != 0 {
            return einval;
        }

        match sqe.opcode {
            XRING_OP_NOP => {
                // NOP reads no argument fields, so all must be zero.
                if sqe.len != 0 || sqe.off != 0 || sqe.slot != 0 || sqe.handle != 0 {
                    return einval;
                }
                0
            }
            // Unknown, or defined but not yet implemented.
            _ => einval,
        }
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
