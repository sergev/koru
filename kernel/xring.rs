// SPDX-License-Identifier: GPL-2.0

//! xring: an experimental non-POSIX kernel API designed for coroutines.
//!
//! Submission and completion are separate events, because a coroutine must yield
//! to its executor between the call and the answer. See `Plan.md` in the
//! repository root for the design.
//!
//! This is T3: `/dev/xring` exists, each open gets a per-fd [`RingCtx`], and the
//! ring can be configured once with `SETUP`. There is no submission path and no
//! arena yet.

mod xring_abi;

use kernel::{
    fs::File,
    ioctl::{_IOC_NR, _IOC_SIZE, _IOC_TYPE},
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    new_mutex,
    prelude::*,
    sync::{Arc, ArcBorrow, Mutex},
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

/// Module state: just the device registration.
///
/// `MiscDeviceRegistration` deregisters in its `Drop`, so unload needs no
/// explicit teardown.
#[pin_data(PinnedDrop)]
struct XringModule {
    #[pin]
    _miscdev: MiscDeviceRegistration<RingCtx>,
}

impl kernel::InPlaceModule for XringModule {
    fn init(_module: &'static ThisModule) -> impl PinInit<Self, Error> {
        // `pr_info!` already prefixes the module name, so do not repeat it.
        pr_info!("init, registering /dev/xring\n");

        // `MiscDeviceOptions` carries no mode, so `miscdevice.mode` stays zero
        // and the node is created 0600 root:root. That is the intended
        // permission model for now; see Plan.md.
        let options = MiscDeviceOptions { name: c"xring" };

        try_pin_init!(Self {
            _miscdev <- MiscDeviceRegistration::register(options),
        })
    }
}

// `#[pin_data]` generates its own `Drop`, so the unload hook must go through
// `PinnedDrop` rather than a plain `impl Drop`.
#[pinned_drop]
impl PinnedDrop for XringModule {
    fn drop(self: Pin<&mut Self>) {
        pr_info!("exit\n");
    }
}

/// The configuration a successful `SETUP` established, kept kernel-private.
///
/// Held separately from the wire struct so the kernel never reads back values
/// it once copied from userspace.
#[derive(Copy, Clone)]
struct RingConfig {
    sq_entries: u32,
    cq_entries: u32,
    slot_size: u32,
    slot_count: u32,
    arena_size: u64,
}

/// Per-fd ring context, the root of the object graph.
///
/// Plan.md's Lifetimes section is the reference for what lands here next: the
/// arena, the captured credential, the state spinlock for submission, the
/// handle table and the completion `CondVar`.
///
/// `config` is a `Mutex` rather than a `SpinLock` because nothing on this path
/// is atomic context. The ring `SpinLock` arrives with the submission state in
/// T4 and covers different fields.
#[pin_data]
struct RingCtx {
    #[pin]
    config: Mutex<Option<RingConfig>>,
}

#[vtable]
impl MiscDevice for RingCtx {
    type Ptr = Arc<Self>;

    fn open(_file: &File, _misc: &MiscDeviceRegistration<Self>) -> Result<Arc<Self>> {
        Arc::pin_init(
            pin_init!(RingCtx {
                config <- new_mutex!(None),
            }),
            GFP_KERNEL,
        )
    }

    fn ioctl(me: ArcBorrow<'_, RingCtx>, _file: &File, cmd: u32, arg: usize) -> Result<isize> {
        // Dispatch on type and command number, never on the whole ioctl number.
        // The number encodes the caller's struct size, so an exact match would
        // report a version skew as "no such ioctl" and hide it.
        if _IOC_TYPE(cmd) != XRING_IOC_TYPE {
            return Err(ENOTTY);
        }

        let ptr = UserPtr::from_addr(arg);
        let size = core::mem::size_of::<XringParams>();

        // Only meaningful for a command we recognise: a size mismatch there
        // means the two sides were built against different ABI revisions.
        let check_size = || {
            if _IOC_SIZE(cmd) == size {
                Ok(())
            } else {
                Err(eproto())
            }
        };

        match _IOC_NR(cmd) {
            XRING_NR_SETUP => {
                check_size()?;
                let (reader, writer) = UserSlice::new(ptr, size).reader_writer();
                me.setup(reader, writer)
            }
            XRING_NR_GET_PARAMS => {
                check_size()?;
                me.get_params(UserSlice::new(ptr, size).writer())
            }
            _ => Err(ENOTTY),
        }?;

        Ok(0)
    }

    // `release` is left as the default, which drops the `Arc`. The refcount is
    // balanced by `ForeignOwnable`: `into_foreign` on open, `from_foreign` on
    // release. In-flight work will hold its own `Arc` from T5 onward, so the
    // last one out frees the context.
}

impl RingCtx {
    /// Fill the fields the kernel always owns: the caps and the ABI identity.
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

    /// Validate a `SETUP` request and derive the configuration it asks for.
    ///
    /// Pure: it takes no lock and mutates nothing, so a rejected request cannot
    /// consume the one-shot.
    fn validate(req: &XringParams) -> Result<RingConfig> {
        // Identity first, so a binding built against a different module gets an
        // unambiguous answer rather than a complaint about some field.
        if req.magic != XRING_MAGIC || req.abi_version != XRING_ABI_VERSION {
            return Err(eproto());
        }

        // Reserved-zero and unknown-flag rejection. This is what allows fields
        // to be added later without breaking old binaries.
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
        // Admission control reserves a CQ slot per consumed SQE, which is what
        // makes CQ overflow unrepresentable. A shallower CQ would break it.
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

        // Both operands are user-controlled `u32`, so this product overflows
        // trivially. Compute it in `u64` and check the result against the cap.
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

        {
            let mut guard = self.config.lock();
            if guard.is_some() {
                return Err(EBUSY);
            }
            *guard = Some(cfg);
        }

        // Write back a struct we built from scratch, never one echoed from
        // userspace, so no unvalidated bytes travel back out.
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

    fn fill_config(p: &mut XringParams, cfg: &RingConfig) {
        p.sq_entries = cfg.sq_entries;
        p.cq_entries = cfg.cq_entries;
        p.slot_size = cfg.slot_size;
        p.slot_count = cfg.slot_count;
        p.arena_size = cfg.arena_size;
        p.configured = 1;
    }
}
