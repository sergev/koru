// SPDX-License-Identifier: MIT
//
// The entry a koru program does not write: the ring, the ambient install, the
// program, the at-exit hooks and the exit status.
//
// A library of its own — not part of libkoru — for two reasons, and the second
// is the load-bearing one. A test binary and the screen daemon have a `main`
// of their own; and `run_main` *calls* `koru_main`, so an object carrying it
// inside libkoru would make every binary that links libkoru owe a definition
// of a program entry it may have no use for.

#include <koru/rt.hpp>

#include <cstdio>

namespace koru {

namespace {

const char *entry_name = "koru";

} // namespace

i32 exit_status(const result<i32> &r)
{
    if (r.ok())
        return r.value();
    if (r.error() == Kind::Cancelled)
        return 130; // ^C
    fprintf(stderr, "%s: %s\n", entry_name, r.error().message());
    return 1;
}

int run_main(int argc, char **argv)
{
    const char *who = argc > 0 && argv[0] ? argv[0] : "koru";
    entry_name      = who;

    result<Ring> ring = Ring::with_config(default_config());
    if (!ring) {
        fprintf(stderr, "%s: /dev/koru: %s\n", who, ring.error().message());
        return 1;
    }
    Ring r              = std::move(ring).take();
    result<Arena> arena = r.mmap();
    if (!arena) {
        fprintf(stderr, "%s: mmap: %s\n", who, arena.error().message());
        return 1;
    }

    Executor ex(std::move(r), std::move(arena).take());
    install(ex);
    i32 status = exit_status(ex.run(koru_main(Args::from_env(argc, argv))));
    shutdown();
    return status;
}

} // namespace koru

int main(int argc, char **argv)
{
    return koru::run_main(argc, argv);
}
