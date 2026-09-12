// SPDX-License-Identifier: MIT

//! The single-threaded executor. Its park is `ENTER(min_complete = 1)`.

use crate::op::{self, Stall};
use crate::reactor::{Inner, Stats};
use crate::slab::{Cookie, Slab};
use koru_sys::abi::Cqe;
use koru_sys::{BufPool, BufSlot, Ring, SetupConfig};
use std::future::Future;
use std::io;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Wake, Waker};

type BoxFut = Pin<Box<dyn Future<Output = ()>>>;

/// Generations start at 1, so `Cookie(0)` names no entry. It names the root
/// of `block_on`, which lives on the stack rather than in the task slab.
const ROOT: Cookie = Cookie(0);

struct Task {
    fut: Option<BoxFut>,
    waker: Option<Waker>,
}

/// A queue push, never an inline re-poll: that would drop a sibling future
/// and re-enter the slab the dispatch loop is holding.
struct TaskWaker {
    id: Cookie,
    ready: Arc<Mutex<Vec<Cookie>>>,
}

impl Wake for TaskWaker {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        let mut q = self.ready.lock().expect("ready queue");
        if !q.contains(&self.id) {
            q.push(self.id);
        }
    }
}

struct Exec {
    tasks: std::cell::RefCell<Slab<Task>>,
    ready: Arc<Mutex<Vec<Cookie>>>,
}

impl Exec {
    fn waker(&self, id: Cookie) -> Waker {
        Waker::from(Arc::new(TaskWaker {
            id,
            ready: Arc::clone(&self.ready),
        }))
    }

    fn mark_ready(&self, id: Cookie) {
        let mut q = self.ready.lock().expect("ready queue");
        if !q.contains(&id) {
            q.push(id);
        }
    }

    /// Released before any poll: a self-waking task would otherwise deadlock
    /// on a non-reentrant mutex.
    fn take_ready(&self) -> Vec<Cookie> {
        std::mem::take(&mut *self.ready.lock().expect("ready queue"))
    }

    fn ready_len(&self) -> usize {
        self.ready.lock().expect("ready queue").len()
    }

    fn tasks_left(&self) -> usize {
        self.tasks.borrow().len()
    }

    /// The future comes out of its slot first, so a poll that spawns or
    /// wakes cannot re-enter the task slab.
    fn poll_task(&self, id: Cookie) {
        let taken = {
            let mut tasks = self.tasks.borrow_mut();
            match tasks.get_mut(id) {
                Some(t) => t.fut.take().zip(t.waker.clone()),
                None => None,
            }
        };
        let Some((mut fut, waker)) = taken else {
            return;
        };
        let mut cx = Context::from_waker(&waker);
        let done = fut.as_mut().poll(&mut cx).is_ready();
        if done {
            let removed = self.tasks.borrow_mut().remove(id);
            drop(removed);
            drop(fut);
            return;
        }
        let orphan = {
            let mut tasks = self.tasks.borrow_mut();
            match tasks.get_mut(id) {
                Some(t) => {
                    t.fut = Some(fut);
                    None
                }
                None => Some(fut),
            }
        };
        drop(orphan);
    }
}

/// A ring, its arena and the executor over them. Cheap to clone; every clone
/// names the same runtime. Not `Send`, deliberately.
#[derive(Clone)]
pub struct Runtime {
    inner: Rc<Inner>,
    exec: Rc<Exec>,
}

impl Runtime {
    /// Open `/dev/koru`, configure it and map the arena.
    pub fn new(cfg: &SetupConfig) -> io::Result<Runtime> {
        let ring = Ring::with_config(cfg)?;
        let arena = ring.mmap()?;
        Ok(Runtime::from_parts(ring, BufPool::new(arena)))
    }

    pub fn from_parts(ring: Ring, pool: BufPool) -> Runtime {
        Runtime {
            inner: Rc::new(Inner::new(ring, pool)),
            exec: Rc::new(Exec {
                tasks: std::cell::RefCell::new(Slab::new()),
                ready: Arc::new(Mutex::new(Vec::new())),
            }),
        }
    }

    pub(crate) fn inner(&self) -> &Rc<Inner> {
        &self.inner
    }

    pub fn ring(&self) -> &Ring {
        self.inner.ring()
    }

    pub fn pool(&self) -> &BufPool {
        self.inner.pool()
    }

    /// A slot from the pool, or `None` when every slot is out.
    pub fn acquire(&self) -> Option<BufSlot> {
        self.inner.pool().acquire()
    }

    pub fn stats(&self) -> Stats {
        self.inner.stats()
    }

    /// Reap until nothing is in flight. Also runs on drop.
    pub fn drain(&self) {
        self.inner.drain();
    }

    /// Detached, fire and forget. T21's ambient `spawn` wraps this.
    pub fn spawn<F: Future<Output = ()> + 'static>(&self, fut: F) {
        let id = self.exec.tasks.borrow_mut().insert(Task {
            fut: Some(Box::pin(fut)),
            waker: None,
        });
        let waker = self.exec.waker(id);
        if let Some(t) = self.exec.tasks.borrow_mut().get_mut(id) {
            t.waker = Some(waker);
        }
        self.exec.mark_ready(id);
    }

    /// Drive until `fut` resolves. Spawned tasks run alongside it.
    pub fn block_on<F: Future>(&self, fut: F) -> F::Output {
        let mut root = Box::pin(fut);
        let root_waker = self.exec.waker(ROOT);
        self.exec.mark_ready(ROOT);
        let mut cq = vec![Cqe::default(); self.inner.cq_len()];

        loop {
            for id in self.exec.take_ready() {
                if id == ROOT {
                    let mut cx = Context::from_waker(&root_waker);
                    if let Poll::Ready(v) = root.as_mut().poll(&mut cx) {
                        return v;
                    }
                } else {
                    self.exec.poll_task(id);
                }
            }
            self.pump(&mut cq);
        }
    }

    /// Drive every spawned task to completion; `block_on` returns as soon as
    /// its own root resolves.
    pub fn run(&self) {
        let mut cq = vec![Cqe::default(); self.inner.cq_len()];
        loop {
            for id in self.exec.take_ready() {
                if id != ROOT {
                    self.exec.poll_task(id);
                }
            }
            if self.exec.tasks_left() == 0 {
                return;
            }
            self.pump(&mut cq);
        }
    }

    /// One turn of the ring, with the foot-gun check in front of it.
    fn pump(&self, cq: &mut [Cqe]) {
        let runnable = self.exec.ready_len();
        if let Some(Stall::NothingCanArrive) =
            op::stall(self.inner.inflight(), self.inner.queued(), runnable)
        {
            panic!(
                "koru: every task is pending with nothing queued and nothing \
                 in flight, so no completion can ever arrive"
            );
        }
        self.inner
            .turn(runnable == 0, None, cq)
            .expect("ENTER failed");
    }
}
