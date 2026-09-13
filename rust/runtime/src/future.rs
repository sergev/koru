// SPDX-License-Identifier: MIT

//! One future per opcode, lazy: nothing is registered until the first poll.
//!
//! A buffer-carrying op takes its `BufSlot` by value and returns it with the
//! result, on the error path too — hence `BufResult<T>`.

use crate::exec::Runtime;
use crate::reactor::Inner;
use crate::slab::Cookie;
use crate::vocab::{Error, Result};
use koru_sys::abi::KORU_O_CREAT;
use koru_sys::error::from_res;
use koru_sys::ring::arg_offset;
use koru_sys::{BufSlot, Sqe};
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll};

/// A result that gives the slot back either way.
pub type BufResult<T> = (Result<T>, BufSlot);

/// An open file, `(index, generation)` as the kernel encodes it. Its drop
/// closes nothing: a destructor cannot await.
#[derive(Copy, Clone, PartialEq, Eq, Hash, Debug)]
pub struct Handle(pub u32);

/// Longest path the kernel takes. Checked here for a clearer error.
const PATH_MAX: usize = 4096;

fn err(e: koru_sys::Errno) -> Error {
    Error::from_errno(e)
}

/// Submit on the first poll, then wait.
macro_rules! poll_body {
    ($self:ident, $cx:ident, $submit:expr) => {{
        if $self.cookie.is_none() {
            $self.cookie = Some($submit);
        }
        let cookie = $self.cookie.expect("just submitted");
        match $self.inner.poll_op(cookie, $cx.waker()) {
            Some(landed) => {
                $self.cookie = None;
                Poll::Ready(landed)
            }
            None => Poll::Pending,
        }
    }};
}

/// Marks the entry abandoned; frees nothing the kernel can still touch.
macro_rules! abandon_on_drop {
    ($ty:ident $(<$lt:lifetime>)?) => {
        impl $(<$lt>)? Drop for $ty $(<$lt>)? {
            fn drop(&mut self) {
                if let Some(cookie) = self.cookie.take() {
                    self.inner.abandon(cookie);
                }
            }
        }
    };
}

// ---------------------------------------------------------------------------
// Ops with no buffer
// ---------------------------------------------------------------------------

#[must_use = "a koru op does nothing until it is awaited"]
pub struct Nop {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
}

impl Future for Nop {
    type Output = Result<()>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let (res, _, _) = match poll_body!(me, cx, me.inner.register(None, |c| Sqe::nop(c.0))) {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        Poll::Ready(from_res(res).map(|_| ()).map_err(err))
    }
}

abandon_on_drop!(Nop);

#[must_use = "a koru op does nothing until it is awaited"]
pub struct Delay {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    ns: u64,
}

impl Future for Delay {
    type Output = Result<()>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let ns = me.ns;
        let (res, _, _) =
            match poll_body!(me, cx, me.inner.register(None, |c| Sqe::delay_ns(c.0, ns))) {
                Poll::Ready(v) => v,
                Poll::Pending => return Poll::Pending,
            };
        Poll::Ready(from_res(res).map(|_| ()).map_err(err))
    }
}

abandon_on_drop!(Delay);

#[must_use = "a koru op does nothing until it is awaited"]
pub struct Close {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    handle: Handle,
}

impl Future for Close {
    type Output = Result<()>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let h = me.handle.0;
        let (res, _, _) = match poll_body!(me, cx, me.inner.register(None, |c| Sqe::close(c.0, h)))
        {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        Poll::Ready(from_res(res).map(|_| ()).map_err(err))
    }
}

abandon_on_drop!(Close);

// ---------------------------------------------------------------------------
// Ops that name a slot
// ---------------------------------------------------------------------------

#[must_use = "a koru op does nothing until it is awaited"]
pub struct Checksum {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    args: Option<(BufSlot, u64, u32)>,
}

impl Future for Checksum {
    type Output = BufResult<u64>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let landed = poll_body!(me, cx, {
            let (slot, off, len) = me.args.take().expect("args survive to the first poll");
            let index = slot.index();
            me.inner
                .register(Some(slot), |c| Sqe::checksum(c.0, index, off, len))
        });
        let (res, _, slot) = match landed {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        let slot = slot.expect("a checksum op owns its slot");
        Poll::Ready((from_res(res).map_err(err), slot))
    }
}

abandon_on_drop!(Checksum);

#[must_use = "a koru op does nothing until it is awaited"]
pub struct Read {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    args: Option<(Handle, BufSlot, u64, u32)>,
}

impl Future for Read {
    type Output = BufResult<usize>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let landed = poll_body!(me, cx, {
            let (handle, slot, off, len) = me.args.take().expect("args survive to the first poll");
            let index = slot.index();
            me.inner
                .register(Some(slot), |c| Sqe::read(c.0, handle.0, index, off, len))
        });
        let (res, _, slot) = match landed {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        let slot = slot.expect("a read op owns its slot");
        // End of file stays `res == 0`; mapping it is T30's job.
        Poll::Ready((from_res(res).map(|n| n as usize).map_err(err), slot))
    }
}

abandon_on_drop!(Read);

#[must_use = "a koru op does nothing until it is awaited"]
pub struct Write {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    args: Option<(Handle, BufSlot, u64, u32)>,
}

impl Future for Write {
    type Output = BufResult<usize>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let landed = poll_body!(me, cx, {
            let (handle, slot, off, len) = me.args.take().expect("args survive to the first poll");
            let index = slot.index();
            me.inner
                .register(Some(slot), |c| Sqe::write(c.0, handle.0, index, off, len))
        });
        let (res, _, slot) = match landed {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        let slot = slot.expect("a write op owns its slot");
        // A short write is a result, not an error; `write_all` loops.
        Poll::Ready((from_res(res).map(|n| n as usize).map_err(err), slot))
    }
}

abandon_on_drop!(Write);

#[must_use = "a koru op does nothing until it is awaited"]
pub struct Adopt {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    fd: i32,
}

impl Future for Adopt {
    type Output = Result<Handle>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let fd = me.fd;
        let (res, _, _) =
            match poll_body!(me, cx, me.inner.register(None, |c| Sqe::adopt_fd(c.0, fd))) {
                Poll::Ready(v) => v,
                Poll::Pending => return Poll::Pending,
            };
        Poll::Ready(from_res(res).map(|h| Handle(h as u32)).map_err(err))
    }
}

abandon_on_drop!(Adopt);

#[must_use = "a koru op does nothing until it is awaited"]
pub struct Open<'a> {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    args: Option<(BufSlot, &'a str, u32, u64)>,
}

impl Future for Open<'_> {
    type Output = BufResult<Handle>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();

        // The kernel checks the same things and answers a bare EINVAL.
        if me.cookie.is_none() {
            let (slot, path, flags, _) = me.args.as_ref().expect("args survive to the first poll");
            let need = if flags & KORU_O_CREAT != 0 {
                arg_offset(0, path.len() as u32) as usize + size_of::<u64>()
            } else {
                path.len()
            };
            let bad = path.is_empty()
                || path.len() >= PATH_MAX
                || need > slot.len()
                || path.as_bytes().contains(&0);
            if bad {
                let (slot, ..) = me.args.take().expect("checked above");
                return Poll::Ready((Err(err(koru_sys::error::EINVAL)), slot));
            }
        }

        let landed = poll_body!(me, cx, {
            let (mut slot, path, flags, mode) =
                me.args.take().expect("args survive to the first poll");
            let len = path.len() as u32;
            slot[..path.len()].copy_from_slice(path.as_bytes());
            if flags & KORU_O_CREAT != 0 {
                let at = arg_offset(0, len) as usize;
                slot[at..at + size_of::<u64>()].copy_from_slice(&mode.to_ne_bytes());
            }
            let index = slot.index();
            me.inner
                .register(Some(slot), |c| Sqe::open(c.0, index, 0, len, flags))
        });
        let (res, _, slot) = match landed {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        let slot = slot.expect("an open op owns its slot");
        Poll::Ready((from_res(res).map(|h| Handle(h as u32)).map_err(err), slot))
    }
}

abandon_on_drop!(Open<'a>);

#[must_use = "a koru op does nothing until it is awaited"]
pub struct PollAdd {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    handle: Handle,
    events: u32,
}

impl Future for PollAdd {
    type Output = Result<u32>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let (h, events) = (me.handle.0, me.events);
        let landed = poll_body!(
            me,
            cx,
            me.inner.register(None, |c| Sqe::poll_add(c.0, h, events))
        );
        let (res, _, _) = match landed {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        // Always non-negative on success, and 0 where nothing asked for can come.
        Poll::Ready(from_res(res).map(|m| m as u32).map_err(err))
    }
}

abandon_on_drop!(PollAdd);

#[must_use = "a koru op does nothing until it is awaited"]
pub struct Stat {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    args: Option<(Handle, BufSlot, u64, u32)>,
}

impl Future for Stat {
    type Output = BufResult<(usize, u64)>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let landed = poll_body!(me, cx, {
            let (handle, slot, off, len) = me.args.take().expect("args survive to the first poll");
            let index = slot.index();
            me.inner
                .register(Some(slot), |c| Sqe::stat(c.0, handle.0, index, off, len))
        });
        let (res, extra, slot) = match landed {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        let slot = slot.expect("a stat op owns its slot");
        let out = from_res(res).map(|n| (n as usize, extra)).map_err(err);
        Poll::Ready((out, slot))
    }
}

abandon_on_drop!(Stat);

#[must_use = "a koru op does nothing until it is awaited"]
pub struct ReadDir {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    args: Option<(Handle, BufSlot, u64, u32)>,
}

impl Future for ReadDir {
    type Output = BufResult<(usize, u64)>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let landed = poll_body!(me, cx, {
            let (handle, slot, off, len) = me.args.take().expect("args survive to the first poll");
            let index = slot.index();
            me.inner
                .register(Some(slot), |c| Sqe::readdir(c.0, handle.0, index, off, len))
        });
        let (res, extra, slot) = match landed {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        let slot = slot.expect("a readdir op owns its slot");
        // `res` 0 is end of directory, as on `READ`; `extra` is the next cookie.
        let out = from_res(res).map(|n| (n as usize, extra)).map_err(err);
        Poll::Ready((out, slot))
    }
}

abandon_on_drop!(ReadDir);

/// Every path op, which differ only in opcode and in what the caller put in the
/// slot. `handle` is zero on all but `MKDIR`, which spends it on the mode.
#[must_use = "a koru op does nothing until it is awaited"]
pub struct PathOp {
    inner: Rc<Inner>,
    cookie: Option<Cookie>,
    args: Option<(u8, BufSlot, u64, u32, u32)>,
}

impl Future for PathOp {
    type Output = BufResult<(usize, u64)>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let landed = poll_body!(me, cx, {
            let (op, slot, off, len, handle) =
                me.args.take().expect("args survive to the first poll");
            let index = slot.index();
            me.inner.register(Some(slot), |c| {
                let mut sqe = Sqe::path(op, c.0, index, off, len);
                sqe.handle = handle;
                sqe
            })
        });
        let (res, extra, slot) = match landed {
            Poll::Ready(v) => v,
            Poll::Pending => return Poll::Pending,
        };
        let slot = slot.expect("a path op owns its slot");
        let out = from_res(res).map(|n| (n as usize, extra)).map_err(err);
        Poll::Ready((out, slot))
    }
}

abandon_on_drop!(PathOp);

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------
//
// Methods, not free functions: T21 owns the ambient ones, T30 the Braam
// names, and a receiver keeps them apart.

impl Runtime {
    pub fn nop(&self) -> Nop {
        Nop {
            inner: Rc::clone(self.inner()),
            cookie: None,
        }
    }

    pub fn delay(&self, ns: u64) -> Delay {
        Delay {
            inner: Rc::clone(self.inner()),
            cookie: None,
            ns,
        }
    }

    pub fn close(&self, handle: Handle) -> Close {
        Close {
            inner: Rc::clone(self.inner()),
            cookie: None,
            handle,
        }
    }

    pub fn checksum(&self, slot: BufSlot, off: u64, len: u32) -> Checksum {
        Checksum {
            inner: Rc::clone(self.inner()),
            cookie: None,
            args: Some((slot, off, len)),
        }
    }

    /// `off` is a file offset; the destination is always slot offset 0.
    pub fn read(&self, handle: Handle, slot: BufSlot, off: u64, len: u32) -> Read {
        Read {
            inner: Rc::clone(self.inner()),
            cookie: None,
            args: Some((handle, slot, off, len)),
        }
    }

    /// `off` is a file offset and must be 0 on anything unseekable; the source
    /// is always slot offset 0. `len` must not be 0.
    pub fn write(&self, handle: Handle, slot: BufSlot, off: u64, len: u32) -> Write {
        Write {
            inner: Rc::clone(self.inner()),
            cookie: None,
            args: Some((handle, slot, off, len)),
        }
    }

    /// A handle for a descriptor the caller already holds. Grants no authority
    /// the caller lacks, and takes a reference of its own.
    pub fn adopt(&self, fd: i32) -> Adopt {
        Adopt {
            inner: Rc::clone(self.inner()),
            cookie: None,
            fd,
        }
    }

    /// The slot carries the path and comes back unchanged in shape. `flags`
    /// are koru's own `KORU_O_*`, not the host `O_*`; `mode` is read only when
    /// they include `KORU_O_CREAT`, and goes after the path as an argument.
    pub fn open<'a>(&self, slot: BufSlot, path: &'a str, flags: u32, mode: u64) -> Open<'a> {
        Open {
            inner: Rc::clone(self.inner()),
            cookie: None,
            args: Some((slot, path, flags, mode)),
        }
    }

    /// Wait once for one of `events`. Single-shot, and a file on no waitqueue
    /// completes at once with 0.
    pub fn poll_add(&self, handle: Handle, events: u32) -> PollAdd {
        PollAdd {
            inner: Rc::clone(self.inner()),
            cookie: None,
            handle,
            events,
        }
    }

    /// `off` is a within-slot offset and must be 8-aligned; `len` is the
    /// caller's buffer size. The answer is `(bytes written, field mask)`.
    pub fn stat(&self, handle: Handle, slot: BufSlot, off: u64, len: u32) -> Stat {
        Stat {
            inner: Rc::clone(self.inner()),
            cookie: None,
            args: Some((handle, slot, off, len)),
        }
    }

    /// `off` is a resume cookie, 0 for the beginning; the answer is
    /// `(bytes written, next cookie)` and 0 bytes is end of directory.
    pub fn readdir(&self, handle: Handle, slot: BufSlot, off: u64, len: u32) -> ReadDir {
        ReadDir {
            inner: Rc::clone(self.inner()),
            cookie: None,
            args: Some((handle, slot, off, len)),
        }
    }

    /// The caller has already put the path at `off` and any argument after it.
    pub fn path_op(&self, op: u8, slot: BufSlot, off: u64, len: u32, handle: u32) -> PathOp {
        PathOp {
            inner: Rc::clone(self.inner()),
            cookie: None,
            args: Some((op, slot, off, len, handle)),
        }
    }
}
