// SPDX-License-Identifier: MIT
//
// Op state and its transitions, pure and generic in the slot type so the state
// machine is testable with no device. `Queued` against `Live` is the
// distinction that matters: the kernel has not seen a queued SQE, so dropping
// one is free, while a live op is owed exactly one completion by C1.

#ifndef KORU_OP_HPP
#define KORU_OP_HPP

#include <coroutine>
#include <cstdint>
#include <optional>
#include <utility>

namespace koru {

enum class State {
    /// In our batch; the kernel has not seen it.
    Queued,
    /// `ENTER` consumed it, so C1 promises exactly one completion.
    Live,
    /// The completion landed before the frame was resumed.
    Ready,
    /// The frame was destroyed while live; discard the completion.
    Abandoned,
    /// The frame was destroyed while queued; the flush drops its SQE.
    Dead,
    /// A fire-and-forget `CANCEL`; its own completion is discarded.
    CancelProbe,
};

/// What a completion means, decided before anything is moved.
enum class Landed {
    /// Store the result and resume whoever is waiting.
    Woke,
    /// Nobody is waiting: remove the entry and drop its slot.
    Discard,
    /// A completion for an entry that cannot have one. A bug here, not there.
    Impossible,
};

/// What destroying the frame means, acted on after the slab lookup.
enum class Abandon {
    /// Queued: take the entry out, drop its SQE at the next flush.
    DropSqe,
    /// Live: leave the entry and its slot in place, submit a `CANCEL`.
    Cancel,
    /// Already complete: take the entry out. A `CANCEL` would be wasted.
    Remove,
    /// Abandoned twice, or a stale cookie.
    Impossible,
};

/// One slab entry. `S` is `BufSlot` in production.
///
/// **The slot lives here, never in the coroutine frame.** A frame can be
/// destroyed with an op in flight; the slab outlives it, and the kernel owns
/// the slot until the CQE lands either way.
template <class S>
struct Op {
    State state = State::Queued;
    std::optional<S> slot;
    std::coroutine_handle<> waiter;
    int64_t res    = 0;
    uint64_t extra = 0;

    static Op queued(std::optional<S> s)
    {
        Op o;
        o.state = State::Queued;
        o.slot  = std::move(s);
        return o;
    }

    static Op probe()
    {
        Op o;
        o.state = State::CancelProbe;
        return o;
    }

    /// The kernel consumed this SQE. Only for the consumed prefix: a short
    /// `submitted` leaves the tail `Queued`.
    void on_submitted()
    {
        if (state == State::Queued)
            state = State::Live;
    }

    Landed on_cqe(int64_t r, uint64_t x)
    {
        switch (state) {
        case State::Live:
            state = State::Ready;
            res   = r;
            extra = x;
            return Landed::Woke;
        case State::Abandoned:
        case State::CancelProbe:
            return Landed::Discard;
        default:
            return Landed::Impossible;
        }
    }

    Abandon on_abandon()
    {
        switch (state) {
        case State::Queued:
            state  = State::Dead;
            waiter = {};
            return Abandon::DropSqe;
        case State::Live:
            state = State::Abandoned;
            // Never resume a handle into a frame that is being destroyed.
            waiter = {};
            return Abandon::Cancel;
        case State::Ready:
            return Abandon::Remove;
        default:
            return Abandon::Impossible;
        }
    }
};

/// Why the executor cannot make progress. Pure, so its truth table is
/// testable with no device.
inline bool nothing_can_arrive(size_t inflight, size_t queued, size_t ready)
{
    return ready == 0 && queued == 0 && inflight == 0;
}

} // namespace koru

#endif // KORU_OP_HPP
