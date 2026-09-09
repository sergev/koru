// SPDX-License-Identifier: GPL-2.0

//! xring: an experimental non-POSIX kernel API designed for coroutines.
//!
//! Submission and completion are separate events, because a coroutine must yield
//! to its executor between the call and the answer. See `Plan.md` in the
//! repository root for the design.
//!
//! This is T2: `/dev/xring` exists and each open gets a per-fd [`RingCtx`].
//! There is no control plane and no data plane yet.

use kernel::{
    fs::File,
    miscdevice::{MiscDevice, MiscDeviceOptions, MiscDeviceRegistration},
    prelude::*,
    sync::Arc,
};

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

/// Per-fd ring context, the root of the object graph.
///
/// Empty for now. Plan.md's Lifetimes section is the reference for what lands
/// here: the arena, the captured credential, the state spinlock, the handle
/// table and the completion `CondVar`. Once any of those pinned types arrive,
/// `open` moves from `Arc::new` to `Arc::pin_init`.
struct RingCtx;

#[vtable]
impl MiscDevice for RingCtx {
    type Ptr = Arc<Self>;

    fn open(_file: &File, _misc: &MiscDeviceRegistration<Self>) -> Result<Arc<Self>> {
        Ok(Arc::new(RingCtx, GFP_KERNEL)?)
    }

    // `release` is left as the default, which drops the `Arc`. The refcount is
    // balanced by `ForeignOwnable`: `into_foreign` on open, `from_foreign` on
    // release. In-flight work will hold its own `Arc` from T5 onward, so the
    // last one out frees the context.
}
