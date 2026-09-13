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
mod tz;
mod vocab;

pub use combinator::{Either, race};
pub use exec::Runtime;
pub use future::{
    Adopt, BufResult, Checksum, Close, Delay, Handle, Nop, Open, PathOp, PollAdd, Read, ReadDir,
    Stat, Write,
};
pub use reactor::Stats;
pub use slab::Cookie;

// Braam's vocabulary over koru-sys's own `Error`, never a second type.
pub use vocab::{Errno, Error, KINDS, Kind, Result, Span, SpanMut, Str};

// Braam's surface: a program names none of the machinery above.
pub use args::Args;
pub use koru_macros::main;
pub use ops::{
    CHUNK, CREATE_MODE, Clock, DirEntry, FileInfo, FileKind, O_ALL, O_APPEND, O_CREATE, O_EXCL,
    O_READ, O_TRUNC, O_WRITE, READ_MAX, SEEK_CUR, SEEK_END, SEEK_MAX, SEEK_SET, clock_now,
    close_fd, copy_file, copy_tree, cwd_get, cwd_set, dup_fd, errln, list_dir, make_dir,
    make_dir_all, make_link, open_at, open_read, read_chunk, read_file, read_link, read_some,
    remove_path, rename_path, seek_fd, sleep_for, stat_fd, stat_of, touch_path, truncate_fd,
    write_all,
};
pub use rt::{at_exit, block_on, install, spawn, stderr, stdin, stdout};

pub use koru_sys::{BufPool, BufSlot, Ring, SetupConfig};
