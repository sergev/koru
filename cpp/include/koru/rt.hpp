// SPDX-License-Identifier: MIT
//
// The ambient ring, the runtime entry and the at-exit hook. The C++ half of
// rust/runtime/src/rt.rs.
//
// A Braam program never names an executor, so the ring is a process-wide
// singleton and the free functions find it. Everything here that looks like a
// POSIX detour — `/proc/self/fd`, the flags in `/proc/self/fdinfo` — is the
// runtime's bounded preamble, and doc/Notes.md says why each one is there.
//
// **`stdin`, `stdout` and `stderr` are macros**, so no identifier here may be
// one of them: the three handles are `in_fd`, `out_fd` and `err_fd`. The Rust
// binding has no such constraint and spells them `stdin()` and its kin; this
// is the one place the two surfaces differ by a name rather than by a type,
// and doc/Notes.md records it.

#ifndef KORU_RT_HPP
#define KORU_RT_HPP

#include <koru/args.hpp>
#include <koru/exec.hpp>
#include <koru/vocab.hpp>

#include <functional>

namespace koru {

namespace detail {

/// Forget the cached standard streams. A new ring means new handles, so a
/// `File` built over the old ones names nothing. Defined by file.cpp.
void reset_std();

/// The screen daemon's byte channel, adopted as stdout, or 0 where there is
/// none to have. Defined by screen.cpp.
Handle adopt_byte_channel(Executor &ex);

} // namespace detail

/// A 64 KiB slot is Braam's own read budget, and eight of them is half a
/// megabyte of arena — enough that a program never waits for one.
SetupConfig default_config();

// ---------------------------------------------------------------------------
// The ambient ring
// ---------------------------------------------------------------------------

/// Make `ex` this process's executor and adopt the standard streams into it.
/// Replaces any previous one, which is what lets a test install its own.
void install(Executor &ex);

Executor *try_current();
Executor &current();
Reactor &reactor();

/// Run the at-exit hooks, drop the spawned tasks and clear the ring. A task
/// still waiting on an op is cancelled rather than waited for, which is
/// Braam's rule: the program ends when the root task returns.
void shutdown();

/// `0` where the descriptor could not be adopted — never a valid handle, so
/// the kernel answers `EBADF` rather than this guessing.
Handle in_fd();
Handle out_fd();
Handle err_fd();

/// True where `h` is the screen's byte channel. It is a socket, so nothing
/// about the handle itself says it is a console; only this does.
bool is_screen(Handle h);

/// Braam's `proc_spawn`, without its eight-task ceiling: the program ends when
/// the root task returns, whatever the others are doing.
void spawn(task<void> t);

/// Run `f` after the program's own task resolves, last registered first.
/// A destructor cannot await, so this is where a buffered writer flushes.
void at_exit(std::function<task<void>()> f);

/// Drive `t` on the ambient ring. Spawned tasks run alongside it.
template <class T>
T block_on(task<T> t)
{
    return current().run(std::move(t));
}

// ---------------------------------------------------------------------------
// Stream positions
// ---------------------------------------------------------------------------
//
// Userspace's own bookkeeping: `READ` and `WRITE` carry an explicit offset and
// never touch `f_pos`, so a stream's position lives here. `seek_fd` exposes it.
//
// The reference count is what makes `dup_fd` possible with no `DUP` opcode:
// both names are the one handle, so the offset is shared and the `CLOSE` waits
// for the last of them. The path is what makes `truncate_fd` possible with
// only a path-named `TRUNCATE`; see doc/Notes.md for what that costs.

/// Where the next write to `h` goes. An unregistered handle counts as a
/// stream: `off` 0, which is the only offset an unseekable file accepts.
uint64_t position(Handle h);
void advance(Handle h, uint64_t n);
void seek_to(Handle h, uint64_t off);
bool is_seekable(Handle h);
bool is_writable(Handle h);

/// The path `open_at` named, for the operations koru has only by path.
Option<String> handle_path(Handle h);

/// Called wherever a handle is created, because only its creator knows whether
/// an offset means anything on it.
void register_handle(Handle h, bool seekable);
void register_opened(Handle h, bool seekable, bool writable, Option<String> path);

/// One more name for the same handle. False where there is no record, which is
/// every handle the runtime did not create.
bool retain_handle(Handle h);

/// Drops one name. True when the kernel handle should now be closed, which an
/// unrecorded handle also is: nothing else is holding it.
bool release_handle(Handle h);

void forget_handle(Handle h);

// ---------------------------------------------------------------------------
// The entry
// ---------------------------------------------------------------------------

/// Braam's exit status, out of what the program returned: a value is itself,
/// `Cancelled` is `^C` and so 130, and every other failure is 1 with a
/// diagnostic. The Rust binding's `Exit` trait, as a function.
i32 exit_status(const result<i32> &r);

/// What `main` does: open the ring, run the program, run the hooks, and hand
/// back the exit status. `cpp/src/main.cpp` is the `main` that calls it, and
/// it is a library of its own so a test binary can have a `main` of its own.
int run_main(int argc, char **argv);

} // namespace koru

/// The program's entry, at global scope so a source that includes only
/// `koru/braam.hpp` can define it unqualified.
///
/// It returns a `Result`, not a bare `i32` as this task's sketch had it:
/// `CO_TRY` returns an `Error` from the function it is written in, so an entry
/// that cannot carry one is an entry no Braam program can use the macros in.
koru::task<koru::result<koru::i32>> koru_main(koru::Args args);

#endif // KORU_RT_HPP
