// SPDX-License-Identifier: MIT
//
// The executor: a reactor, the tasks it owns, and a `run()` whose park is one
// `ENTER`.
//
// The ready queue is the reactor's: a frame becomes ready when its CQE lands,
// and `pump` resumes it after the dispatch loop rather than inside it. What
// the executor adds is ownership of spawned frames and the decision of when to
// park — including the refusal to park on a ring where nothing can arrive,
// which is the same foot-gun the kernel's `ENTER` closes on its own side.

#ifndef KORU_EXEC_HPP
#define KORU_EXEC_HPP

#include <koru/reactor.hpp>
#include <koru/task.hpp>

#include <coroutine>
#include <cstdint>
#include <vector>

namespace koru {

class Executor {
public:
    Executor(Ring ring, Arena arena) : reactor_(std::move(ring), std::move(arena)) {}
    ~Executor();

    Executor(const Executor &)            = delete;
    Executor &operator=(const Executor &) = delete;

    Reactor &reactor() { return reactor_; }
    BufPool &pool() { return reactor_.pool(); }

    /// Braam's `proc_spawn`: start it now, and own the frame until it ends.
    /// The task runs to its first suspension point before this returns, so an
    /// op it submits is queued by the time the caller's next line runs — which
    /// is what lets a program arm three timers and then go and do something
    /// else.
    void spawn(task<void> t);

    /// How many spawned tasks are still running.
    size_t spawned() const;

    /// Drive `t` to completion, parking on `ENTER` in between.
    ///
    /// Aborts rather than spinning where nothing can arrive: a root task that
    /// is not done with no op in flight, nothing queued and nothing ready is
    /// waiting for something that does not exist, and a silent spin is the
    /// worst way to say so.
    template <class T>
    T run(task<T> t)
    {
        if (!t)
            fail("koru::Executor::run: a task whose frame could not be allocated");
        t.handle().resume();
        while (!t.done())
            turn();
        if constexpr (!std::is_void_v<T>)
            return t.await_resume();
    }

    /// Run everything spawned to completion. What an at-exit hook needs, and
    /// what makes "the program ends when the root task returns" a choice
    /// rather than an accident.
    void drain();

    /// One turn: reap what is there, resume what is ready, and park if there
    /// is anything to park for.
    void turn();

    /// How long one park may last. A cap, never a deadline: `min_complete`
    /// decides whether `ENTER` waits at all.
    void set_park_timeout(uint64_t ns) { park_ns_ = ns; }

private:
    void reap_finished();

    Reactor reactor_;
    std::vector<std::coroutine_handle<>> owned_;
    uint64_t park_ns_ = 0;
};

/// Adopt a descriptor the process already has, re-opening it first where koru
/// could not take it as it stands: the kernel admits a non-regular file only
/// when it was opened non-blocking, and koru never sets that bit on a
/// descriptor it did not open. The re-open is ordinary POSIX — the bounded
/// preamble, as the Rust runtime's is.
///
/// A negative return is `-errno`, as every `res` is.
task<int64_t> adopt_stream(Reactor &r, int fd);

} // namespace koru

#endif // KORU_EXEC_HPP
