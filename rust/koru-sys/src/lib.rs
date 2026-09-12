// SPDX-License-Identifier: MIT

//! Raw bindings for `/dev/koru`: the ABI structs, the ioctl wrappers, the ring
//! and its arena, the buffer pool, and the errno table.
//!
//! No dependencies. The futures, the executor and the Braam surface are the
//! `koru` crate, on top of this one. See doc/Notes.md.

pub mod abi;
pub mod error;
pub mod pool;
pub mod ring;
pub mod sys;

pub use abi::{Cqe, KoruEnter, KoruParams, Sqe};
pub use error::{Errno, Error, Kind};
pub use pool::{BufPool, BufSlot};
pub use ring::{Arena, EnterError, Entered, Progress, Ring, SetupConfig};
