// SPDX-License-Identifier: MIT

#include <koru/exec.hpp>

#include <fcntl.h>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace koru {

Executor::~Executor()
{
    drop_tasks();
}

void Executor::drop_tasks()
{
    // A spawned task still suspended has an op the reactor knows about; its
    // frame is ours, so it goes here. The reactor's slab entry survives until
    // its CQE lands, which is exactly what the abandon path is for.
    for (std::coroutine_handle<> h : owned_)
        if (h)
            h.destroy();
    owned_.clear();
}

void Executor::spawn(task<void> t)
{
    if (!t)
        fail("koru::Executor::spawn: a task whose frame could not be allocated");
    std::coroutine_handle<> h = t.release();
    owned_.push_back(h);
    h.resume(); // to its first suspension point, and no further
    reap_finished();
}

size_t Executor::spawned() const
{
    size_t n = 0;
    for (std::coroutine_handle<> h : owned_)
        if (h)
            n++;
    return n;
}

void Executor::reap_finished()
{
    for (std::coroutine_handle<> &h : owned_) {
        if (h && h.done()) {
            h.destroy();
            h = {};
        }
    }
    while (!owned_.empty() && !owned_.back())
        owned_.pop_back();
}

void Executor::turn()
{
    // Nothing in flight, nothing queued, nothing ready: `ENTER` would return
    // at once rather than sleeping, and this would spin for ever.
    size_t ready = reactor_.deferred();
    if (nothing_can_arrive(reactor_.inflight(), reactor_.queued(), ready))
        fail("koru::Executor: waiting for something that cannot arrive");
    // A frame is ready to run, so this turn must not wait for a completion:
    // `min_complete` is what decides whether `ENTER` sleeps at all.
    reactor_.pump(ready ? 0 : 1, park_ns_);
    reap_finished();
}

void Executor::drain()
{
    while (spawned() > 0)
        turn();
}

task<int64_t> adopt_stream(Reactor &r, int fd)
{
    struct stat st = {};
    bool regular = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);

    int use = fd;
    int reopened = -1;
    if (!regular) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        int flags = fcntl(fd, F_GETFL);
        if (flags < 0)
            flags = O_RDWR;
        int mode = flags & O_ACCMODE;
        // A file description of our own, so O_NONBLOCK never reaches the one
        // the parent shares.
        reopened = ::open(path, mode | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
        if (reopened >= 0)
            use = reopened;
    }

    Completion c = co_await r.submit(sqe::adopt_fd(0, use));
    if (reopened >= 0)
        ::close(reopened); // ADOPT_FD took a reference of its own
    co_return c.res;
}

} // namespace koru
