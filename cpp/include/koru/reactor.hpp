// SPDX-License-Identifier: MIT
//
// The reactor: the ring, the op slab, and the batch of SQEs waiting to go.
// `op_awaiter` is the coroutine's end of it.
//
// C++20 coroutines are continuation-based, which is what a completion ring
// wants: `await_suspend` hands the reactor a `std::coroutine_handle` and the
// reactor resumes it when the CQE lands. No polling, no waker indirection, no
// spurious wakeups.
//
// Two rules, both of which doc/Notes.md argues:
//
//   - **The slot lives in the slab, never in the frame.** A frame destroyed
//     mid-flight takes nothing the kernel is using with it.
//   - **`await_suspend` must not touch `this` after publishing the handle.**
//     Once the handle is visible the coroutine may already have been resumed
//     and its frame destroyed. Harmless under a single-threaded executor,
//     fatal the moment a waiter thread appears — so the rule is written down
//     now, not then.

#ifndef KORU_REACTOR_HPP
#define KORU_REACTOR_HPP

#include <koru/op.hpp>
#include <koru/pool.hpp>
#include <koru/ring.hpp>
#include <koru/slab.hpp>

#include <coroutine>
#include <cstdint>
#include <vector>

namespace koru {

/// What an awaited op hands back: the CQE's two halves, and the slot the
/// operation borrowed.
struct Completion {
    int64_t res    = 0;
    uint64_t extra = 0;
    BufSlot slot;

    /// `res` as a result, with the `< 0` sign convention applied.
    result<uint64_t, Errno> value() const { return from_res(res); }
};

class Reactor;

/// The awaiter an operation is awaited through. It lives inside the coroutine
/// frame, so its destructor is where a destroyed frame is noticed.
class op_awaiter {
public:
    op_awaiter(Reactor *reactor, Cookie cookie) : reactor_(reactor), cookie_(cookie) {}
    ~op_awaiter();

    // It is a temporary inside one `co_await`, and nothing may copy or move it
    // while the reactor holds a pointer into the frame it sits in.
    op_awaiter(const op_awaiter &)            = delete;
    op_awaiter &operator=(const op_awaiter &) = delete;
    op_awaiter(op_awaiter &&)                 = delete;
    op_awaiter &operator=(op_awaiter &&)      = delete;

    /// True where the completion has already landed — an inline op reaped by
    /// the same `ENTER` that submitted it, which `NOP`, `OPEN` and `CLOSE`
    /// make the common case rather than a corner.
    bool await_ready() const;
    void await_suspend(std::coroutine_handle<> h);
    Completion await_resume();

private:
    Reactor *reactor_ = nullptr;
    Cookie cookie_;
    bool taken_ = false;
};

class Reactor {
public:
    Reactor(Ring ring, Arena arena) : ring_(std::move(ring)), pool_(std::move(arena)) {}

    Reactor(const Reactor &)            = delete;
    Reactor &operator=(const Reactor &) = delete;

    Ring &ring() { return ring_; }
    BufPool &pool() { return pool_; }

    /// Queue one op. `sqe.user_data` is overwritten with the cookie: it is the
    /// reactor's name for the op and nothing else may use that field.
    op_awaiter submit(koru_sqe sqe, BufSlot slot);
    op_awaiter submit(koru_sqe sqe) { return submit(sqe, BufSlot()); }

    /// One `ENTER`: flush the batch, reap up to `min_complete` completions,
    /// and resume every frame whose op landed. Returns how many CQEs arrived.
    ///
    /// Resumption happens after the dispatch loop, never inside it: a resumed
    /// frame can submit, abandon and pump again, and the slab must not be
    /// walked while that happens.
    uint32_t pump(uint32_t min_complete, uint64_t timeout_ns = 0);

    /// Ops the kernel has and has not answered yet: what C1 still owes. For
    /// the tests and for the stall predicate.
    size_t inflight() const { return inflight_; }
    size_t queued() const { return pending_.size(); }
    size_t live() const { return slab_.live(); }

    /// `ENTER` calls made. Measured, never assumed: one per pump is what the
    /// ABI provides, and a binding that pays two per op passes every
    /// behavioural test there is.
    uint64_t enters() const { return enters_; }

    // ------------------------------------------------------------ internals

    /// The awaiter's three calls, kept here because the state lives here.
    bool is_ready(Cookie c) const;
    void park(Cookie c, std::coroutine_handle<> h);
    Completion take(Cookie c);
    void abandon(Cookie c);

private:
    std::vector<koru_sqe> ready_batch();

    Ring ring_;
    BufPool pool_;
    Slab<Op<BufSlot>> slab_;
    std::vector<koru_sqe> pending_;
    std::vector<koru_cqe> cq_;
    std::vector<std::coroutine_handle<>> woken_;
    size_t inflight_ = 0;
    uint64_t enters_ = 0;
};

} // namespace koru

#endif // KORU_REACTOR_HPP
