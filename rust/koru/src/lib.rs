// SPDX-License-Identifier: MIT

//! Futures, an op slab and a single-threaded executor over `koru-sys`, and
//! Braam's surface over those.
//!
//! The rule the crate exists to enforce: the slab entry owns the `BufSlot`,
//! the future owns only its cookie, and dropping a future frees nothing. The
//! reasoning is in doc/Notes.md; the borrow discipline is in `reactor.rs`.

#![forbid(unsafe_code)]

mod args;
mod combinator;
mod exec;
mod future;
mod op;
mod ops;
mod reactor;
pub mod rt;
mod slab;
mod vocab;

pub use combinator::{Either, race};
pub use exec::Runtime;
pub use future::{Adopt, BufResult, Checksum, Close, Delay, Handle, Nop, Open, Read, Write};
pub use reactor::Stats;
pub use slab::Cookie;

// Braam's vocabulary over koru-sys's own `Error`, never a second type.
pub use vocab::{Errno, Error, KINDS, Kind, Result, Span, SpanMut, Str};

// Braam's surface: a program names none of the machinery above.
pub use args::Args;
pub use koru_macros::main;
pub use ops::{close_fd, write_all};
pub use rt::{at_exit, block_on, install, spawn, stderr, stdin, stdout};

pub use koru_sys::{BufPool, BufSlot, Ring, SetupConfig};
