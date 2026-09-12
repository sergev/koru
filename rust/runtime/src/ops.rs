// SPDX-License-Identifier: MIT

//! The write half of Braam's operation layer. The rest arrives at T30.
//!
//! Braam's signatures, unchanged: a descriptor, what to write, and a `Result`.
//! Each is a slot acquisition, a submit, an await and a copy out.

use crate::future::Handle;
use crate::rt;
use crate::vocab::{Error, Result, Str};
use koru_sys::BufSlot;
use koru_sys::error::EAGAIN;
use std::future::poll_fn;
use std::task::Poll;

/// How long to wait for a full pipe to drain. `POLL_ADD` replaces this at T22;
/// until then a non-blocking write that says `EAGAIN` has nothing to wait on.
const EAGAIN_BACKOFF_NS: u64 = 1_000_000;

/// Writes all of `s`, retrying a short write.
///
/// The offset is userspace's own bookkeeping, because `WRITE` never touches
/// `f_pos`: a seekable handle advances, a stream stays at 0.
pub async fn write_all(fd: Handle, s: Str<'_>) -> Result<()> {
    let rt = rt::current();
    let mut left = s.as_bytes();

    // The kernel refuses a zero-length write, and Braam's write_all of an
    // empty string is a no-op rather than an error.
    while !left.is_empty() {
        let mut slot = acquire().await;
        let n = left.len().min(slot.len());
        slot[..n].copy_from_slice(&left[..n]);

        let off = rt::position(fd);
        let (res, back) = rt.write(fd, slot, off, n as u32).await;
        drop(back);

        match res {
            Ok(0) => return Err(Error::closed()),
            Ok(wrote) => {
                rt::advance(fd, wrote as u64);
                left = &left[wrote..];
            }
            Err(e) if e.raw() == EAGAIN => rt.delay(EAGAIN_BACKOFF_NS).await?,
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

/// Retires the handle. Braam's returns nothing: there is no answer a program
/// could act on, and the kernel frees the file either way.
pub async fn close_fd(fd: Handle) {
    let _ = rt::current().close(fd).await;
    rt::forget_handle(fd);
}

/// A slot, waiting for one if the pool is out. Yielding is enough because a
/// slot comes back when some other task's op completes, which is progress the
/// executor makes on its own; a waiter queue arrives with T30.
async fn acquire() -> BufSlot {
    let rt = rt::current();
    poll_fn(move |cx| match rt.acquire() {
        Some(slot) => Poll::Ready(slot),
        None => {
            cx.waker().wake_by_ref();
            Poll::Pending
        }
    })
    .await
}
