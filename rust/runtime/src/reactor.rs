// SPDX-License-Identifier: MIT

//! The op slab, the pending batch, and the one place that calls `ENTER`.
//!
//! **Lock order is `tasks > slab > pending`, and no borrow is held across a
//! poll, a wake, or the drop of a payload.** Every borrow below is scoped to a
//! block that decides; the doing happens after it is released.

use crate::op::{Abandon, Landed, Op, State};
use crate::slab::{Cookie, Slab};
use koru_sys::abi::Cqe;
use koru_sys::error::{self, Errno};
use koru_sys::{BufPool, BufSlot, Ring, Sqe};
use std::cell::{Cell, RefCell};
use std::task::Waker;
use std::time::Duration;

/// Counters and gauges. The only way a test can see any of this.
#[derive(Copy, Clone, Debug, Default, PartialEq, Eq)]
pub struct Stats {
    pub enters: u64,
    pub sqes_submitted: u64,
    pub cqes_reaped: u64,
    pub cancels_submitted: u64,
    /// Parks a signal interrupted. The writeback survives one.
    pub eintrs: u64,
    pub inflight: usize,
    pub free_slots: usize,
    pub slab_live: usize,
}

pub(crate) struct Inner {
    ring: Ring,
    pool: BufPool,
    slab: RefCell<Slab<Op<BufSlot>>>,
    pending: RefCell<Vec<Sqe>>,
    inflight: Cell<usize>,
    counters: Cell<Stats>,
}

impl Inner {
    pub fn new(ring: Ring, pool: BufPool) -> Inner {
        Inner {
            ring,
            pool,
            slab: RefCell::new(Slab::new()),
            pending: RefCell::new(Vec::new()),
            inflight: Cell::new(0),
            counters: Cell::new(Stats::default()),
        }
    }

    pub fn ring(&self) -> &Ring {
        &self.ring
    }

    pub fn pool(&self) -> &BufPool {
        &self.pool
    }

    pub fn cq_len(&self) -> usize {
        self.ring.params().cq_entries.max(1) as usize
    }

    /// `to_submit` past `sq_entries` fails the ioctl outright, so a batch
    /// bigger than the queue goes out in chunks rather than in one call.
    fn sq_len(&self) -> usize {
        self.ring.params().sq_entries.max(1) as usize
    }

    pub fn inflight(&self) -> usize {
        self.inflight.get()
    }

    pub fn queued(&self) -> usize {
        self.pending.borrow().len()
    }

    pub fn stats(&self) -> Stats {
        Stats {
            inflight: self.inflight.get(),
            free_slots: self.pool.free_count(),
            slab_live: self.slab.borrow().len(),
            ..self.counters.get()
        }
    }

    fn count(&self, f: impl FnOnce(&mut Stats)) {
        let mut s = self.counters.get();
        f(&mut s);
        self.counters.set(s);
    }

    /// Register an op and queue its SQE. Nothing reaches the kernel until
    /// the next turn, so a batch goes out in one `ENTER`.
    pub fn register(&self, slot: Option<BufSlot>, make: impl FnOnce(Cookie) -> Sqe) -> Cookie {
        let cookie = self.slab.borrow_mut().insert(Op::queued(slot));
        self.pending.borrow_mut().push(make(cookie));
        cookie
    }

    /// `Some` means the completion is in: the entry is gone and its slot
    /// comes back with the result.
    pub fn poll_op(&self, cookie: Cookie, waker: &Waker) -> Option<(i64, u64, Option<BufSlot>)> {
        let taken = {
            let mut slab = self.slab.borrow_mut();
            match slab.get(cookie).map(|o| o.state) {
                Some(State::Ready { .. }) => slab.remove(cookie),
                Some(_) => {
                    if let Some(op) = slab.get_mut(cookie) {
                        op.waker = Some(waker.clone());
                    }
                    None
                }
                None => panic!("koru: polled an op whose slab entry is gone"),
            }
        };
        taken.map(|op| match op.state {
            State::Ready { res, extra } => (res, extra, op.slot),
            s => unreachable!("removed a {s:?} entry as ready"),
        })
    }

    /// The `Future::drop` path. It frees nothing the kernel can still touch.
    pub fn abandon(&self, cookie: Cookie) {
        let action = {
            let mut slab = self.slab.borrow_mut();
            match slab.get_mut(cookie) {
                Some(op) => op.on_abandon(),
                None => return,
            }
        };
        match action {
            // The kernel never saw it: the slot goes back now, the entry
            // stays until the next turn drops its SQE.
            Abandon::DropSqe => {
                let slot = {
                    let mut slab = self.slab.borrow_mut();
                    slab.get_mut(cookie).and_then(|o| o.slot.take())
                };
                drop(slot);
            }
            Abandon::Remove => {
                let removed = self.slab.borrow_mut().remove(cookie);
                drop(removed);
            }
            // The slot stays inside the entry until the target's CQE lands.
            Abandon::Cancel => {
                let probe = self.slab.borrow_mut().insert(Op::probe());
                self.pending
                    .borrow_mut()
                    .push(Sqe::cancel(probe.0, cookie.0));
                self.count(|s| s.cancels_submitted += 1);
            }
            Abandon::Impossible => panic!("koru: op abandoned twice"),
        }
    }

    /// Take the batch, dropping the SQEs of ops abandoned before submission.
    fn take_batch(&self) -> Vec<Sqe> {
        let mut batch = std::mem::take(&mut *self.pending.borrow_mut());
        let mut dead = Vec::new();
        {
            let slab = self.slab.borrow();
            batch.retain(|sqe| {
                let c = Cookie(sqe.user_data);
                if let Some(State::Dead) = slab.get(c).map(|o| o.state) {
                    dead.push(c);
                    false
                } else {
                    true
                }
            });
        }
        for c in dead {
            let removed = self.slab.borrow_mut().remove(c);
            drop(removed);
        }
        batch
    }

    fn requeue(&self, tail: &[Sqe]) {
        if !tail.is_empty() {
            self.pending.borrow_mut().splice(0..0, tail.iter().copied());
        }
    }

    /// Only the consumed prefix becomes `Live`: the tail must stay `Queued`
    /// or its drop would cancel an op the kernel never saw.
    fn mark_live(&self, consumed: &[Sqe]) {
        let mut slab = self.slab.borrow_mut();
        for sqe in consumed {
            if let Some(op) = slab.get_mut(Cookie(sqe.user_data)) {
                op.on_submitted();
            }
        }
        self.inflight.set(self.inflight.get() + consumed.len());
        drop(slab);
        self.count(|s| s.sqes_submitted += consumed.len() as u64);
    }

    /// Route completions. Returns how many futures were woken.
    fn dispatch(&self, cqes: &[Cqe]) -> usize {
        let mut woke = 0;
        for cqe in cqes {
            self.inflight.set(self.inflight.get().saturating_sub(1));
            let cookie = Cookie(cqe.user_data);
            let landed = {
                let mut slab = self.slab.borrow_mut();
                slab.get_mut(cookie).map(|op| op.on_cqe(cqe.res, cqe.extra))
            };
            match landed {
                // A retired generation: the cookie did its job.
                None => continue,
                Some(Landed::Woke) => {
                    let waker = {
                        let mut slab = self.slab.borrow_mut();
                        slab.get_mut(cookie).and_then(|op| op.waker.take())
                    };
                    // Outside the borrow: a wake must not re-enter the slab.
                    if let Some(w) = waker {
                        w.wake();
                        woke += 1;
                    }
                }
                Some(Landed::Discard) => {
                    let removed = self.slab.borrow_mut().remove(cookie);
                    drop(removed);
                }
                Some(Landed::Impossible) => {
                    panic!(
                        "koru: completion {:#x} for an op that cannot have one",
                        cqe.user_data
                    )
                }
            }
        }
        self.count(|s| s.cqes_reaped += cqes.len() as u64);
        woke
    }

    /// Submit what is queued, reap what is there, and wait only if waiting
    /// can achieve anything.
    pub fn turn(&self, wait: bool, timeout: Option<Duration>, cq: &mut [Cqe]) -> Result<(), Errno> {
        let batch = self.take_batch();
        let mut off = 0usize;
        let mut woke = 0usize;
        let mut blocked;

        let chunk = self.sq_len();
        loop {
            let tail = &batch[off..batch.len().min(off + chunk)];
            // Ask for the completion on the **submitting** call: `ENTER`
            // submits and waits in one, which is what makes one op one syscall
            // rather than two. What is in `tail` is in flight by the time the
            // kernel tests `min_complete`, so an op about to be submitted
            // counts as one that can answer.
            //
            // Nothing queued and nothing running means nothing can arrive, and
            // then the kernel returns at once rather than waiting; asking is
            // still pointless, so this does not.
            let idle = self.inflight.get() == 0;
            let want = u32::from(wait && woke == 0 && (!idle || !tail.is_empty()));

            let (progress, errno) = match self.ring.enter(tail, cq, want, timeout) {
                Ok(e) => (e.progress, None),
                Err(e) => (e.progress, Some(e.errno)),
            };
            self.count(|s| s.enters += 1);

            let submitted = progress.submitted as usize;
            if submitted > 0 {
                self.mark_live(&batch[off..off + submitted]);
                off += submitted;
            }
            let completed = progress.completed as usize;
            if completed > 0 {
                woke += self.dispatch(&cq[..completed]);
            }

            match errno {
                None => {}
                // The writeback is valid on EINTR: resume from it.
                Some(e) if e == error::EINTR => self.count(|s| s.eintrs += 1),
                Some(e) => {
                    self.requeue(&batch[off..]);
                    return Err(e);
                }
            }

            blocked = submitted == 0 && !tail.is_empty();
            let done = off == batch.len() && (!wait || woke > 0 || self.inflight.get() == 0);
            if done || (blocked && self.inflight.get() == 0) {
                break;
            }
        }

        self.requeue(&batch[off..]);
        Ok(())
    }

    /// Reap until nothing is in flight, so the free count means something.
    /// Bounded: a queued hour-long delay must not make a drop hang.
    pub fn drain(&self) {
        let mut cq = vec![Cqe::default(); self.cq_len()];
        for _ in 0..64 {
            if self.inflight.get() == 0 && self.pending.borrow().is_empty() {
                return;
            }
            if self
                .turn(true, Some(Duration::from_millis(200)), &mut cq)
                .is_err()
            {
                return;
            }
        }
    }
}

impl Drop for Inner {
    fn drop(&mut self) {
        self.drain();
    }
}
