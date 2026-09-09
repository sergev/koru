// SPDX-License-Identifier: GPL-2.0

//! xring: an experimental non-POSIX kernel API designed for coroutines.
//!
//! Submission and completion are separate events, because a coroutine must yield
//! to its executor between the call and the answer. See `Plan.md` in the
//! repository root for the design.
//!
//! This is the T1 skeleton: it proves the out-of-tree build and the load path.
//! Nothing of the actual interface exists yet.

use kernel::prelude::*;

module! {
    type: Xring,
    name: "xring",
    authors: ["Serge Vakulenko"],
    description: "Coroutine-oriented async syscall interface (skeleton)",
    license: "GPL",
}

/// Module state. Empty for now; from T2 this owns the `MiscDevice` registration.
struct Xring;

impl kernel::Module for Xring {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        // `pr_info!` already prefixes the module name, so do not repeat it.
        pr_info!("init\n");
        Ok(Xring)
    }
}

impl Drop for Xring {
    fn drop(&mut self) {
        pr_info!("exit\n");
    }
}
