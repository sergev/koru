// SPDX-License-Identifier: MIT

#include <koru/rt.hpp>

#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace koru {

namespace {

/// What a handle's creator knew about it.
struct Pos {
    bool seekable = false;
    uint64_t off  = 0;
    uint32_t refs = 1;
    bool writable = false;
    Option<String> path;
};

struct Ambient {
    Executor *ex = nullptr;
    Handle std_[3]{ 0, 0, 0 };
    /// The screen's byte channel, where stdout is one. 0 otherwise.
    Handle screen = 0;
    std::unordered_map<Handle, Pos> pos;
};

Ambient &amb()
{
    static Ambient a;
    return a;
}

std::vector<std::function<task<void>()>> &hooks()
{
    static std::vector<std::function<task<void>()>> v;
    return v;
}

Pos *find(Handle h)
{
    auto it = amb().pos.find(h);
    return it == amb().pos.end() ? nullptr : &it->second;
}

String fd_path(int fd)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "/proc/self/fd/%d", fd);
    return String(buf);
}

/// Adopt descriptor `fd`, re-opening it first where koru could not use it as
/// it stands: the kernel admits a non-regular file only when it was opened
/// non-blocking, and koru never sets that bit on a descriptor it did not open.
task<int64_t> adopt_std(Reactor &r, int fd, bool *seekable)
{
    struct stat st = {};
    *seekable      = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);

    int use      = fd;
    int reopened = -1;
    int flags    = fcntl(fd, F_GETFL);
    if (!*seekable && flags >= 0 && (flags & O_NONBLOCK) == 0) {
        // A file description of our own, so O_NONBLOCK never reaches the one
        // the parent shares. Fails on anything /proc/self/fd cannot re-open —
        // the virtio console a VM is run on is one — and then the raw
        // descriptor is adopted and a write to it is refused, which is honest.
        reopened = ::open(fd_path(fd).c_str(), (flags & O_ACCMODE) | O_NONBLOCK | O_NOCTTY);
        if (reopened >= 0)
            use = reopened;
    }

    Completion c = co_await r.submit(sqe::adopt_fd(0, use));
    if (reopened >= 0)
        ::close(reopened); // ADOPT_FD took a reference of its own
    co_return c.res;
}

} // namespace

// `detail::reset_std` and `detail::adopt_byte_channel` are declared in rt.hpp
// and defined by file.cpp and screen.cpp: they are the two places `install`
// and `shutdown` reach up into the layers above this one.

SetupConfig default_config()
{
    SetupConfig cfg;
    cfg.sq_entries   = 64;
    cfg.cq_entries   = 128;
    cfg.slot_size    = 64 * 1024;
    cfg.slot_count   = 8;
    cfg.handle_count = 64;
    return cfg;
}

// ---------------------------------------------------------------------------
// The ambient ring
// ---------------------------------------------------------------------------

void install(Executor &ex)
{
    detail::reset_std();
    Ambient &a = amb();
    a.ex       = &ex;
    a.screen   = 0;
    a.pos.clear();
    a.std_[0] = a.std_[1] = a.std_[2] = 0;

    for (int fd = 0; fd < 3; fd++) {
        // stdout is the screen's byte channel where there is one to have, so
        // the terminal koru was started from is never adopted in its place.
        Handle h      = fd == 1 ? detail::adopt_byte_channel(ex) : 0;
        bool seekable = false;
        if (h != 0) {
            a.screen = h;
        } else {
            int64_t res = ex.run(adopt_std(ex.reactor(), fd, &seekable));
            h           = res > 0 ? Handle(res) : 0;
        }
        if (h != 0) {
            // No path: nothing here was opened by name, so `truncate_fd`
            // refuses a standard stream as `seek_fd` refuses one.
            register_opened(h, seekable, fd != 0, std::nullopt);
        }
        a.std_[fd] = h;
    }
}

Executor *try_current()
{
    return amb().ex;
}

Executor &current()
{
    Executor *ex = amb().ex;
    if (!ex)
        fail("koru: no ambient ring: call koru::install, or use koru_main");
    return *ex;
}

Reactor &reactor()
{
    return current().reactor();
}

void shutdown()
{
    while (!hooks().empty()) {
        std::function<task<void>()> h = std::move(hooks().back());
        hooks().pop_back();
        if (Executor *ex = try_current())
            ex->run(h());
    }
    if (Executor *ex = try_current())
        ex->drop_tasks();
    detail::reset_std();
    amb().ex = nullptr;
    amb().pos.clear();
    amb().screen = 0;
}

Handle in_fd()
{
    return amb().std_[0];
}

Handle out_fd()
{
    return amb().std_[1];
}

Handle err_fd()
{
    return amb().std_[2];
}

Handle std_fd(Handle h)
{
    return h < 3 ? amb().std_[h] : h;
}

bool is_screen(Handle h)
{
    return h != 0 && amb().screen == h;
}

void spawn(task<void> t)
{
    current().spawn(std::move(t));
}

void at_exit(std::function<task<void>()> f)
{
    hooks().push_back(std::move(f));
}

// ---------------------------------------------------------------------------
// Stream positions
// ---------------------------------------------------------------------------

uint64_t position(Handle h)
{
    const Pos *p = find(h);
    return p && p->seekable ? p->off : 0;
}

void advance(Handle h, uint64_t n)
{
    if (Pos *p = find(h))
        p->off += n;
}

void seek_to(Handle h, uint64_t off)
{
    if (Pos *p = find(h))
        p->off = off;
}

bool is_seekable(Handle h)
{
    const Pos *p = find(h);
    return p && p->seekable;
}

bool is_writable(Handle h)
{
    const Pos *p = find(h);
    return p && p->writable;
}

Option<String> handle_path(Handle h)
{
    const Pos *p = find(h);
    return p ? p->path : std::nullopt;
}

void register_handle(Handle h, bool seekable)
{
    register_opened(h, seekable, false, std::nullopt);
}

void register_opened(Handle h, bool seekable, bool writable, Option<String> path)
{
    if (!amb().ex)
        return;
    Pos p;
    p.seekable   = seekable;
    p.off        = 0;
    p.refs       = 1;
    p.writable   = writable;
    p.path       = std::move(path);
    amb().pos[h] = std::move(p);
}

bool retain_handle(Handle h)
{
    Pos *p = find(h);
    if (!p)
        return false;
    p->refs++;
    return true;
}

bool release_handle(Handle h)
{
    Pos *p = find(h);
    if (!p)
        return true;
    if (p->refs > 1) {
        p->refs--;
        return false;
    }
    amb().pos.erase(h);
    return true;
}

void forget_handle(Handle h)
{
    amb().pos.erase(h);
}

} // namespace koru
