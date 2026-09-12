// SPDX-License-Identifier: MIT

//! Futures, an op slab and a single-threaded executor over `koru-sys`.
//!
//! The rule the crate exists to enforce: the slab entry owns the `BufSlot`,
//! the future owns only its cookie, and dropping a future frees nothing. The
//! reasoning is in doc/Notes.md; the borrow discipline is in `reactor.rs`.

#![forbid(unsafe_code)]

mod exec;
mod future;
mod op;
mod reactor;
mod slab;

pub use exec::Runtime;
pub use future::{BufResult, Checksum, Close, Delay, Handle, Nop, Open, Read};
pub use reactor::Stats;
pub use slab::Cookie;

// The vocabulary is koru-sys's, unchanged. T20 adds the `Result` alias and the
// Braam aliases over this same `Error`, never a second type.
pub use koru_sys::error::{Errno, Error, Kind};
pub use koru_sys::{BufPool, BufSlot, Ring, SetupConfig};
