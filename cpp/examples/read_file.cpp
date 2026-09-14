// SPDX-License-Identifier: MIT
//
// T42's done test: T15's demo, in C++.
//
// A file read through the ring while three timers armed longest-first complete
// out of order, printed with `WRITE` rather than with `printf`. The bytes on
// stdout are the file and nothing else, and the completion order goes to
// stderr — both byte-identical to `rust/runtime/examples/read_file.rs`, which
// scripts/cpp.sh checks by running the two and comparing.
//
// Nothing here names an `ENTER`, a slab or a cookie. It does name the reactor,
// because the ambient ring and Braam's surface are T44 and T45.

#include <koru/exec.hpp>

#include <koru/task.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace koru;

namespace {

constexpr uint64_t MS = 1000000;

/// One timer, recording when it finished rather than when it was armed.
task<void> timer(Reactor &r, uint64_t ms, std::vector<uint64_t> *order)
{
    co_await r.submit(sqe::delay_ns(0, ms * MS));
    order->push_back(ms);
}

/// Everything the demo writes goes through the ring, so stdout and stderr are
/// koru handles rather than descriptors.
task<int64_t> write_all(Reactor &r, int64_t handle, const std::string &text)
{
    size_t at = 0;
    while (at < text.size()) {
        BufSlot slot = r.pool().acquire();
        if (!slot.held())
            co_return -ENOMEM;
        size_t n = text.size() - at;
        if (n > slot.size())
            n = slot.size();
        memcpy(slot.bytes().data(), text.data() + at, n);
        // The index before the move: the order in which a call's arguments are
        // evaluated is unspecified, so reading it from the moved-from slot in
        // the same expression is a hazard C++ will not warn about.
        uint32_t index = slot.index();
        Completion c = co_await r.submit(
            sqe::write(0, uint32_t(handle), index, at, uint32_t(n)), std::move(slot));
        if (c.res <= 0)
            co_return c.res;
        at += size_t(c.res);
    }
    co_return int64_t(at);
}

task<int> demo(Executor &ex, const char *path, std::vector<uint64_t> *order)
{
    Reactor &r = ex.reactor();

    // Armed longest first, so submission order and completion order differ.
    for (uint64_t ms : { uint64_t(30), uint64_t(10), uint64_t(20) })
        ex.spawn(timer(r, ms, order));

    int64_t out = co_await adopt_stream(r, 1);
    int64_t err = co_await adopt_stream(r, 2);
    if (out < 0 || err < 0) {
        fprintf(stderr, "read_file: stdout or stderr could not be adopted\n");
        co_return 1;
    }

    BufSlot slot = r.pool().acquire();
    if (!slot.held())
        co_return 1;
    {
        std::span<uint8_t> b = slot.bytes();
        memset(b.data(), 0, b.size());
        memcpy(b.data(), path, strlen(path));
    }
    uint32_t plen = uint32_t(strlen(path));
    uint32_t index = slot.index();
    Completion o =
        co_await r.submit(sqe::open(0, index, 0, plen, KORU_O_RDONLY), std::move(slot));
    if (o.res <= 0) {
        fprintf(stderr, "read_file: %s: %s\n", path, Errno(int(-o.res)).message());
        co_return 1;
    }
    int64_t h = o.res;

    BufSlot into = std::move(o.slot);
    uint32_t len = uint32_t(into.size());
    index = into.index();
    Completion rd =
        co_await r.submit(sqe::read(0, uint32_t(h), index, 0, len), std::move(into));
    if (rd.res < 0) {
        fprintf(stderr, "read_file: read: %s\n", Errno(int(-rd.res)).message());
        co_return 1;
    }
    std::string text(reinterpret_cast<const char *>(rd.slot.bytes().data()), size_t(rd.res));
    rd.slot.release();
    co_await r.submit(sqe::close(0, uint32_t(h)));

    if (co_await write_all(r, out, text) < 0)
        co_return 1;

    // The timers ran while the file was being opened and read; this waits for
    // the stragglers without re-entering the executor that is already running.
    while (order->size() < 3)
        co_await r.submit(sqe::delay_ns(0, MS));

    std::string line;
    for (size_t i = 0; i < order->size(); i++) {
        if (i)
            line += ' ';
        line += std::to_string((*order)[i]);
    }
    line += '\n';
    if (co_await write_all(r, err, line) < 0)
        co_return 1;
    co_return 0;
}

} // namespace

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/etc/hosts";

    SetupConfig cfg;
    cfg.sq_entries    = 64;
    cfg.cq_entries    = 128;
    cfg.slot_size     = 64 * 1024;
    cfg.slot_count    = 8;
    cfg.handle_count  = 64;
    result<Ring> ring = Ring::with_config(cfg);
    if (!ring) {
        fprintf(stderr, "read_file: /dev/koru: %s\n", ring.error().message());
        return 1;
    }
    Ring r              = std::move(ring).take();
    result<Arena> arena = r.mmap();
    if (!arena) {
        fprintf(stderr, "read_file: mmap: %s\n", arena.error().message());
        return 1;
    }

    Executor ex(std::move(r), std::move(arena).take());
    std::vector<uint64_t> order;
    int status = ex.run(demo(ex, path, &order));
    ex.drain();
    return status;
}
