// SPDX-License-Identifier: MIT

#include <koru/reactor.hpp>

#include <utility>

namespace koru {

// ---------------------------------------------------------------------------
// op_awaiter
// ---------------------------------------------------------------------------

op_awaiter::~op_awaiter()
{
    // Resumed and read: the entry is gone already. Otherwise this frame is
    // being destroyed with an op the kernel may still be working on.
    if (!taken_ && reactor_)
        reactor_->abandon(cookie_);
}

bool op_awaiter::await_ready() const
{
    return reactor_->is_ready(cookie_);
}

void op_awaiter::await_suspend(std::coroutine_handle<> h)
{
    // Nothing may touch `this` after the handle is published: the coroutine
    // can be resumed — and its frame destroyed — the moment the reactor has
    // it. Single-threaded today; the rule is what keeps a waiter thread from
    // being a rewrite.
    reactor_->park(cookie_, h);
}

Completion op_awaiter::await_resume()
{
    taken_ = true;
    return reactor_->take(cookie_);
}

// ---------------------------------------------------------------------------
// Reactor
// ---------------------------------------------------------------------------

op_awaiter Reactor::submit(koru_sqe sqe, BufSlot slot)
{
    std::optional<BufSlot> held;
    if (slot.held())
        held = std::move(slot);
    Cookie c      = slab_.insert(Op<BufSlot>::queued(std::move(held)));
    sqe.user_data = c.value;
    pending_.push_back(sqe);
    return op_awaiter(this, c);
}

bool Reactor::is_ready(Cookie c) const
{
    const Op<BufSlot> *op = slab_.get(c);
    return op && op->state == State::Ready;
}

void Reactor::park(Cookie c, std::coroutine_handle<> h)
{
    if (Op<BufSlot> *op = slab_.get(c))
        op->waiter = h;
}

Completion Reactor::take(Cookie c)
{
    Completion out;
    Op<BufSlot> *op = slab_.get(c);
    if (!op)
        fail("koru::Reactor: await_resume on an op that is not there");
    out.res   = op->res;
    out.extra = op->extra;
    if (op->slot)
        out.slot = std::move(*op->slot);
    slab_.remove(c);
    return out;
}

void Reactor::abandon(Cookie c)
{
    Op<BufSlot> *op = slab_.get(c);
    if (!op)
        return; // never submitted, or taken already
    switch (op->on_abandon()) {
    case Abandon::DropSqe:
        // The kernel has not seen it. `flush` drops an SQE whose cookie names
        // nothing live, so the entry simply goes.
        slab_.remove(c);
        break;
    case Abandon::Cancel: {
        // Live: the entry and its slot stay until the CQE lands, and the
        // completion is discarded when it does. The CANCEL is best-effort —
        // -ENOENT means the op finished first, which is not an error.
        koru_sqe s   = sqe::cancel(0, c.value);
        Cookie probe = slab_.insert(Op<BufSlot>::probe());
        s.user_data  = probe.value;
        pending_.push_back(s);
        cancels_++;
        break;
    }
    case Abandon::Remove:
        slab_.remove(c);
        break;
    case Abandon::Impossible:
        fail("koru::Reactor: an op abandoned twice");
    }
}

/// The batch to hand the kernel, with the dropped SQEs filtered out.
///
/// An SQE whose entry is gone was abandoned while queued: the kernel never
/// sees it, which is the whole value of the Queued state.
std::vector<koru_sqe> Reactor::ready_batch()
{
    std::vector<koru_sqe> go;
    go.reserve(pending_.size());
    for (const koru_sqe &s : pending_)
        if (slab_.get(Cookie(s.user_data)))
            go.push_back(s);
    pending_.clear();
    return go;
}

uint32_t Reactor::pump(uint32_t min_complete, uint64_t timeout_ns)
{
    std::vector<koru_sqe> go = ready_batch();
    if (cq_.size() < 64)
        cq_.resize(64);

    // **One** ENTER, carrying the batch and the reap together. The ABI
    // provides exactly that, and a submit call followed by a reap call would
    // be two syscalls per op — which is the bug the Rust executor had until
    // its ENTER count was measured rather than assumed.
    enters_++;
    EnterResult r = ring_.enter(go, cq_, min_complete, timeout_ns);
    if (!r) {
        // The ioctl itself failed, which is a protocol failure rather than an
        // op's result: nothing was consumed, so the batch goes back.
        pending_.assign(go.begin(), go.end());
        return 0;
    }

    uint32_t consumed = r.value().progress.submitted;
    for (uint32_t i = 0; i < go.size(); i++) {
        Op<BufSlot> *op = slab_.get(Cookie(go[i].user_data));
        if (!op)
            continue;
        if (i < consumed) {
            op->on_submitted();
            inflight_++;
            sqes_++;
        } else {
            pending_.push_back(go[i]); // admission control stopped short
        }
    }

    uint32_t n = r.value().progress.completed;
    cqes_ += n;
    woken_.clear();
    for (uint32_t i = 0; i < n; i++) {
        Cookie c(cq_[i].user_data);
        Op<BufSlot> *op = slab_.get(c);
        if (!op)
            continue; // a completion for an entry already taken out
        Landed landed = op->on_cqe(cq_[i].res, cq_[i].extra);
        if (landed != Landed::Impossible)
            inflight_--;
        switch (landed) {
        case Landed::Woke:
            if (op->waiter)
                woken_.push_back(op->waiter);
            break;
        case Landed::Discard:
            // The abandoned op's slot goes back here, and not one moment
            // earlier: the kernel owned it until this CQE.
            slab_.remove(c);
            break;
        case Landed::Impossible:
            fail("koru::Reactor: a completion for an op that cannot have one");
        }
    }

    // After the loop, never inside it: a resumed frame submits, abandons and
    // pumps again, and none of that may happen while the slab is being walked.
    std::vector<std::coroutine_handle<>> resume = std::move(woken_);
    woken_.clear();
    for (std::coroutine_handle<> h : resume)
        if (h && !h.done())
            h.resume();
    return n;
}

} // namespace koru
