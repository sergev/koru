// SPDX-License-Identifier: MIT

//! `race`: the first of two futures to finish, and the other one dropped.
//!
//! Dropping the loser is the point, not a side effect: that is what runs the
//! op future's `Drop` and therefore `Inner::abandon`. doc/Notes.md says why
//! that drop is ordering-safe.

use std::future::{Future, poll_fn};
use std::pin::pin;
use std::task::Poll;

/// Which side of a [`race`] finished first.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum Either<A, B> {
    A(A),
    B(B),
}

impl<T> Either<T, T> {
    /// Both sides the same type: take the value and forget which side it was.
    pub fn into_inner(self) -> T {
        match self {
            Either::A(v) | Either::B(v) => v,
        }
    }
}

/// Poll both until one is ready, then **drop the other**.
///
/// For a koru op that drop is not free: a live op's entry keeps its slot and a
/// `CANCEL` goes out. It happens inside the caller's poll with no slab borrow
/// held, because `poll_task` takes the future out of its slot first.
///
/// Both sides are polled before `Pending`, so neither starves, and which goes
/// first alternates: a fixed order settles every race between two ready
/// futures the same way. doc/Notes.md has what that costs.
pub async fn race<A: Future, B: Future>(a: A, b: B) -> Either<A::Output, B::Output> {
    let mut a = pin!(a);
    let mut b = pin!(b);
    // Flipped before each round, so the first poll of a fresh race takes `a`
    // first, as the argument order reads.
    let mut b_first = true;
    poll_fn(move |cx| {
        b_first = !b_first;
        if b_first {
            if let Poll::Ready(v) = b.as_mut().poll(cx) {
                return Poll::Ready(Either::B(v));
            }
            if let Poll::Ready(v) = a.as_mut().poll(cx) {
                return Poll::Ready(Either::A(v));
            }
        } else {
            if let Poll::Ready(v) = a.as_mut().poll(cx) {
                return Poll::Ready(Either::A(v));
            }
            if let Poll::Ready(v) = b.as_mut().poll(cx) {
                return Poll::Ready(Either::B(v));
            }
        }
        Poll::Pending
    })
    .await
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    use std::pin::Pin;
    use std::rc::Rc;
    use std::task::{Context, Waker};

    /// Ready after `n` polls, and it records that it was polled and dropped.
    struct Witness {
        left: u32,
        tag: u8,
        polls: Rc<Cell<u32>>,
        dropped: Rc<Cell<bool>>,
    }

    impl Future for Witness {
        type Output = u8;

        fn poll(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<u8> {
            let me = self.get_mut();
            me.polls.set(me.polls.get() + 1);
            if me.left == 0 {
                Poll::Ready(me.tag)
            } else {
                me.left -= 1;
                Poll::Pending
            }
        }
    }

    impl Drop for Witness {
        fn drop(&mut self) {
            self.dropped.set(true);
        }
    }

    struct Probe {
        polls: Rc<Cell<u32>>,
        dropped: Rc<Cell<bool>>,
    }

    impl Probe {
        fn new() -> Probe {
            Probe {
                polls: Rc::new(Cell::new(0)),
                dropped: Rc::new(Cell::new(false)),
            }
        }

        fn fut(&self, left: u32, tag: u8) -> Witness {
            Witness {
                left,
                tag,
                polls: Rc::clone(&self.polls),
                dropped: Rc::clone(&self.dropped),
            }
        }
    }

    /// Drive a future to completion with no executor.
    fn spin<F: Future>(fut: F) -> F::Output {
        let mut fut = pin!(fut);
        // These futures never suspend for real, so nothing has to be woken.
        let waker = Waker::noop();
        let mut cx = Context::from_waker(waker);
        for _ in 0..64 {
            if let Poll::Ready(v) = fut.as_mut().poll(&mut cx) {
                return v;
            }
        }
        panic!("race did not finish in 64 polls");
    }

    #[test]
    fn the_side_that_finishes_first_wins() {
        let (a, b) = (Probe::new(), Probe::new());
        assert_eq!(spin(race(a.fut(0, 1), b.fut(3, 2))), Either::A(1));

        let (a, b) = (Probe::new(), Probe::new());
        assert_eq!(spin(race(a.fut(3, 1), b.fut(0, 2))), Either::B(2));
    }

    /// The whole reason the combinator exists: T16's `CANCEL` comes from here.
    #[test]
    fn the_loser_is_dropped_when_the_race_returns() {
        let (a, b) = (Probe::new(), Probe::new());
        let loser = Rc::clone(&b.dropped);
        {
            let r = spin(race(a.fut(0, 1), b.fut(9, 2)));
            assert_eq!(r, Either::A(1));
        }
        assert!(loser.get(), "the losing future outlived the race");
    }

    #[test]
    fn both_sides_are_polled_before_the_race_suspends() {
        let (a, b) = (Probe::new(), Probe::new());
        let (pa, pb) = (Rc::clone(&a.polls), Rc::clone(&b.polls));
        spin(race(a.fut(2, 1), b.fut(2, 2)));
        assert!(pa.get() >= 2 && pb.get() >= 2, "{} {}", pa.get(), pb.get());
    }

    /// A fixed order would settle every race between two ready futures the
    /// same way, which is exactly the bias T16's test must not have.
    #[test]
    fn the_first_polled_side_alternates() {
        // Both ready at once: the first round takes A first, as written.
        let (a, b) = (Probe::new(), Probe::new());
        assert_eq!(spin(race(a.fut(0, 1), b.fut(0, 2))), Either::A(1));

        // Both pending once, then both ready: the second round decides it, and
        // by then it is B's turn to go first.
        let (a, b) = (Probe::new(), Probe::new());
        assert_eq!(spin(race(a.fut(1, 1), b.fut(1, 2))), Either::B(2));
    }

    #[test]
    fn into_inner_forgets_the_side() {
        assert_eq!(Either::<u8, u8>::A(7).into_inner(), 7);
        assert_eq!(Either::<u8, u8>::B(9).into_inner(), 9);
    }
}
