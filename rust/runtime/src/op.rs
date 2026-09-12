// SPDX-License-Identifier: MIT

//! Op state and its transitions, pure and generic in the slot type so the
//! state machine is testable with no device. `Queued` against `Live` is the
//! distinction that matters; doc/Notes.md says why.

use std::task::Waker;

/// One slab entry. `S` is `BufSlot` in production.
pub(crate) struct Op<S> {
    pub state: State,
    pub slot: Option<S>,
    pub waker: Option<Waker>,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum State {
    /// In our batch; the kernel has not seen it.
    Queued,
    /// `ENTER` consumed it, so C1 promises exactly one completion.
    Live,
    /// The completion landed before the next poll.
    Ready { res: i64, extra: u64 },
    /// The future was dropped while live; discard the completion.
    Abandoned,
    /// The future was dropped while queued; the flush drops its SQE.
    Dead,
    /// A fire-and-forget `CANCEL`; its own completion is discarded.
    CancelProbe,
}

/// What a completion means, decided before any borrow is taken.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Landed {
    /// Store the result and wake whoever is waiting.
    Woke,
    /// Nobody is waiting: remove the entry and drop its slot.
    Discard,
    /// A completion for an entry that cannot have one. A bug here, not there.
    Impossible,
}

/// What a drop means, acted on after the slab borrow is released.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Abandon {
    /// Queued: take the entry out, drop its SQE at the next flush.
    DropSqe,
    /// Live: leave the entry and its slot in place, submit a `CANCEL`.
    Cancel,
    /// Already complete: take the entry out. A `CANCEL` would be wasted.
    Remove,
    /// Dropped twice, or a stale cookie.
    Impossible,
}

impl<S> Op<S> {
    pub fn queued(slot: Option<S>) -> Op<S> {
        Op {
            state: State::Queued,
            slot,
            waker: None,
        }
    }

    pub fn probe() -> Op<S> {
        Op {
            state: State::CancelProbe,
            slot: None,
            waker: None,
        }
    }

    /// The kernel consumed this SQE. Only for the consumed prefix: a short
    /// `submitted` leaves the tail `Queued`.
    pub fn on_submitted(&mut self) {
        if self.state == State::Queued {
            self.state = State::Live;
        }
    }

    pub fn on_cqe(&mut self, res: i64, extra: u64) -> Landed {
        match self.state {
            State::Live => {
                self.state = State::Ready { res, extra };
                Landed::Woke
            }
            State::Abandoned | State::CancelProbe => Landed::Discard,
            _ => Landed::Impossible,
        }
    }

    pub fn on_abandon(&mut self) -> Abandon {
        match self.state {
            State::Queued => {
                self.state = State::Dead;
                Abandon::DropSqe
            }
            State::Live => {
                self.state = State::Abandoned;
                self.waker = None;
                Abandon::Cancel
            }
            State::Ready { .. } => Abandon::Remove,
            _ => Abandon::Impossible,
        }
    }
}

/// Why the executor cannot make progress.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Stall {
    /// Nothing runnable, nothing queued, nothing in flight. Parking here
    /// spins: the kernel returns at once rather than sleeping.
    NothingCanArrive,
}

/// Pure, so its truth table is testable with no device.
pub(crate) fn stall(inflight: usize, queued: usize, ready: usize) -> Option<Stall> {
    if ready == 0 && queued == 0 && inflight == 0 {
        Some(Stall::NothingCanArrive)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn op() -> Op<u32> {
        Op::queued(Some(7))
    }

    #[test]
    fn the_happy_path_is_queued_then_live_then_ready() {
        let mut o = op();
        assert_eq!(o.state, State::Queued);
        o.on_submitted();
        assert_eq!(o.state, State::Live);
        assert_eq!(o.on_cqe(4096, 0), Landed::Woke);
        assert_eq!(
            o.state,
            State::Ready {
                res: 4096,
                extra: 0
            }
        );
    }

    #[test]
    fn dropping_a_queued_op_drops_its_sqe_and_submits_no_cancel() {
        let mut o = op();
        assert_eq!(o.on_abandon(), Abandon::DropSqe);
        assert_eq!(o.state, State::Dead);
    }

    #[test]
    fn dropping_a_live_op_keeps_its_slot_and_asks_for_a_cancel() {
        let mut o = op();
        o.on_submitted();
        assert_eq!(o.on_abandon(), Abandon::Cancel);
        assert_eq!(o.state, State::Abandoned);
        assert!(o.slot.is_some(), "the slot stays until the CQE lands");
    }

    /// OPEN, CLOSE and NOP complete inline inside the submitting ENTER, so
    /// this is the common case, not a corner.
    #[test]
    fn dropping_an_already_ready_op_submits_no_cancel() {
        let mut o = op();
        o.on_submitted();
        o.on_cqe(0, 0);
        assert_eq!(o.on_abandon(), Abandon::Remove);
    }

    #[test]
    fn an_abandoned_op_discards_its_completion() {
        let mut o = op();
        o.on_submitted();
        o.on_abandon();
        assert_eq!(o.on_cqe(-125, 0), Landed::Discard);
    }

    #[test]
    fn a_cancel_probe_discards_its_own_completion() {
        let mut o: Op<u32> = Op::probe();
        assert_eq!(o.on_cqe(0, 0), Landed::Discard);
    }

    #[test]
    fn a_completion_for_a_queued_or_ready_op_is_impossible() {
        let mut o = op();
        assert_eq!(o.on_cqe(0, 0), Landed::Impossible);
        let mut o = op();
        o.on_submitted();
        o.on_cqe(0, 0);
        assert_eq!(o.on_cqe(0, 0), Landed::Impossible, "C1: exactly one CQE");
    }

    #[test]
    fn abandoning_twice_is_impossible() {
        let mut o = op();
        o.on_submitted();
        o.on_abandon();
        assert_eq!(o.on_abandon(), Abandon::Impossible);
    }

    #[test]
    fn the_stall_predicate_fires_only_when_nothing_can_arrive() {
        assert_eq!(stall(0, 0, 0), Some(Stall::NothingCanArrive));
        assert_eq!(stall(1, 0, 0), None, "a completion is coming");
        assert_eq!(stall(0, 1, 0), None, "an SQE is waiting to go out");
        assert_eq!(stall(0, 0, 1), None, "a task is runnable");
        assert_eq!(stall(1, 1, 1), None);
    }
}
