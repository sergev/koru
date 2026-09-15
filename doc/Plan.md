# koru — remaining tasks

Only work still to do. The design, the ABI invariants, the accepted gaps and
everything T0–T12 established are in [Notes.md](Notes.md); read that first.

Each task carries a done test. A task is finished when that test passes **and**
has been shown to fail when the thing it checks is broken. Move what it taught
into [Notes.md](Notes.md), then delete it from here.

**[M]** mechanical · **[R]** risky/exploratory

## The userspace API is Braam's

[Braam](https://github.com/braamix/core) is a browser-hosted OS whose programs
are wasm modules with no libc and no stack switching. Its syscalls are a submit
import plus a completion export, and every blocking call is a C++20 coroutine:
different substrate, the same bet koru makes. koru's userspace API is that API,
minus what this substrate cannot carry. It is documented in that project's
`doc/Programming_Manual.md`, and it already has more than fifty programs written
against it — which is what turns the language-neutrality claim into something
harder to fake than two demos printing the same bytes.

Most of it maps directly. Braam's `Task<T>` is lazy, move-only and uses
symmetric transfer, which T41 below required independently. `Result` with
`TRY`/`CO_TRY` is what Notes.md already proposes. The names are already
idiomatic in both languages. Two things fit better here than on Braam itself:
Rust's `poll` returning ready *is* the awaiter fast path Braam has to simulate,
and that fast path earns more on koru, because a miss costs a syscall rather
than a scheduler step.

Out of scope, permanently: the host services (`fetch_url`, `ws_connect`, `pick`,
`clip_*`, `inflate`, `verify_sig`), and processes, pipes, signals and job
control. Also Braam's freestanding rules — no libc, no namespaces, the 512-byte
frame budget — which are wasm artefacts. `cwd_get` and `cwd_set` use ordinary
POSIX calls and say so in the binding's own documentation.

**The terminal cell grid is in scope**, and an earlier version of this list said
it was not. Phase 9 builds it: a daemon owning a window, reached over a socket
the client adopts into the ring. The rest of the sentence above still stands,
and the argument for a socket rather than a pty depends on it standing.

Two adaptations. `DELAY_NS` caps at one hour while Braam's `sleep_for` takes
milliseconds up to fifty days, so the binding chunks it. And `READ` reports end
of file as `res == 0` where `read_chunk` reports `Err(Closed)`, so the wrapper
maps it.

## Reading this list

Numbers are execution order, and they were renumbered once when the Braam
decision landed. If you renumber again, fix the references in
[README.md](../README.md), [CLAUDE.md](../CLAUDE.md), [Notes.md](Notes.md) and
`test/`.

Rust lives in a Cargo workspace under `rust/`, whose directories are named by
role and whose packages keep the `koru` prefix: `rust/sys` is `koru-sys` and
holds the ABI structs, the ioctl wrappers and `abi_dump`; `rust/runtime` is
`koru` and holds the futures, the executor and the surface; `rust/macros` is
`koru-macros`. C++ lives in `cpp`, with names in `namespace koru` and
`cpp/include/koru/braam.hpp` hoisting them to global scope, so a Braam
source compiles with one added include.

The two bindings share no code. That is the point: the second one exists to show
the ABI is language-neutral, so resist factoring anything across them. The Rust
surface is built first and the C++ surface transcribes it, because the design
will churn and churning it twice is the same mistake this plan avoids for the
ABI.

## Phase 9 — the screen

Built. `koru-screen` is a daemon owning an SDL3 window and a full terminal
emulator; clients reach it over a Unix socket they `ADOPT_FD` into the ring, so
every blit, every key and every printed byte travels through `WRITE`, `READ`
and `POLL_ADD`, and no kernel opcode was added. `ProcScreen` keeps its six
methods and the transport is invisible above it. doc/Notes.md has the design
and what each task taught.

Three extensions stay deferred, and the design is shaped so each is additive
rather than a rewrite. **Multiplexing** — several clients, foreground
switching, and `attach` to a second window — is what the reserved `seq = 0` and
the reserved `KS_OP_TERM_OPEN` leave room for. **A real pty** replacing the
byte channel, which the client cannot distinguish, and which is what would let
a shell run in the window. **Reconnecting** to a restarted daemon, which a
full-screen program would never notice, because the resize path already marks
the whole grid damaged. Add them here as tasks when one of them is wanted.

## Phase 11 — the C++ surface

Built. Braam's whole userspace API sits on T39-T43's core: the vocabulary and
the ambient ring, the operation layer, the buffered stream and its iterators,
the program shell, and the screen client. `cpp/include/koru/braam.hpp` hoists
all of it to global scope, and `hello` and `date` in `cpp/examples/` are each
compared against a Rust twin. Only `hello` is Braam's own source; `date` is
koru's program in Braam's style, which T49 is what found out, and `less` was
too until T49b replaced it with Braam's. doc/Notes.md has the design and what
each task taught,
including the GCC bug that makes `CO_TRY(co_await ...)` clang-only.

## Phase 12 — the proof

Built. Twenty-three of Braam's twenty-four `src/cmd` programs sit in
`cpp/cmd/` with their include block as the only edit.
`scripts/run-portability.sh` compares each of the twenty-one text ones against
coreutils on a fixture tree, under ASan and UBSan, and five of them against
idiomatic-Rust twins byte for byte; `scripts/run-e2e.sh` paints Braam's `less`
beside the Rust pager and compares the two windows, and types at Braam's
`edit` through a scripted keyboard. `tee` is the one that does not port, and
signals were out of scope before the surface was written. doc/Notes.md has
what the real programs found in libkoru — five of them were bugs — the six
places Braam's programs are deliberately not GNU's, and what the full-screen
half cost.

**A program that wants a value out of an await still needs clang**, and none
of the twenty-three writes one: GCC cannot compile a statement expression
holding both a `co_await` and a `co_return`, so `i32 n = CO_TRY(co_await f());`
is an internal compiler error there. `CO_TRY_VOID` is unaffected, and `edit` —
the one most likely to have wanted it — uses only that.

## Phase 13 — only if justified

### T51 [R] — eventfd and tokio bridge

eventfd registration plus a tokio `AsyncFd` bridge. Only if tokio integration
becomes a goal, and try a pure-userspace waiter thread first. This is where
"`await_suspend` must not touch `this`" stops being theoretical — though T22's
wake callback already made a sibling of it real in the kernel.

Done test: a koru op and a tokio TCP read complete in one tokio runtime, with
the koru side making no progress until the eventfd fires. Then the rule itself:
a coroutine resumed from the waiter thread while `await_suspend` is still on the
submitting thread, 100k iterations under TSan, with a deliberately reintroduced
touch of `this` after publishing the handle shown to fail it.

## Verification

Every task gates on its own done test. The end-to-end sequence once everything
lands:

1. `make -C test && scripts/run.sh` — the module's own check, in a VM. Covers
   the fuzz, the kmemleak scan past its minimum object age, the creds cases,
   the lockdep breakages and a clean `rmmod`.
2. `scripts/run-rust.sh` — the Rust suite, in a VM. `(cd rust && cargo test -p
   koru-sys --lib)` is the host-only half: ABI assertions, the ioctl numbers
   and the errno table, with no device needed.
3. `(cd rust && cargo test -p koru --lib)` — the host-only half: the cookie,
   the slab, the op state machine, the stall predicate and the vocabulary
   (T20), with no device. Everything else in that crate needs `/dev/koru` and
   runs as the `runtime` suite inside step 2's boot: futures, executor, drop
   safety and the Rust surface (T15, T16, T21, T30–T32), plus the screen
   client against its fake daemon (T37).
4. The Rust examples (T21), run inside step 2's boot rather than on the host:
   `cargo run` cannot reach `/dev/koru` from here. `scripts/rust.sh` runs each
   three ways, because a pipe, a regular file and an argument are three
   different assertions about the entry.
5. `cmake -B build && cmake --build build`, then `ctest --test-dir build` —
   the host-only C++ half: the slab and the op state machine (T39–T40), the
   symmetric-transfer depth case (T41), the vocabulary and `Args` (T44), the
   option parser and the calendar (T47), and the grid, the text buffer and the
   view (T48). No VM, no device.
6. `scripts/abi.sh` — the two ABI dumps agree (T14). The screen protocol's two
   dumps share the same runner (T33).
7. `scripts/run-cpp.sh` — the C++ device suite, in a VM and under ASan and
   UBSan: the T4–T11 matrix (T39), abandonment (T40), drop safety (T43), the
   operation layer (T45), the buffered stream (T46) and the screen client
   against its fake daemon (T48). It also runs T42's demo, T44's hello world
   and T47's `date` beside their Rust twins and compares the bytes.
8. `ctest --test-dir build -L screen` — the terminal model against Braam's own
   cell-exact tests (T34), the protocol server's rejection matrix through
   `feed()` (T36) and the five pixel oracles (T35). **These need no VM, no
   `/dev/koru` and no display server**, so they are the fastest feedback in the
   whole list and should run first in practice.
9. `scripts/run-e2e.sh` — the daemon end to end (T38), the byte channel
   (T38b), Braam's `less` in both bindings painting a byte-identical window
   (T48, T49b), and Braam's `edit` typed at through the daemon's scripted
   keyboard (T49b).
10. `scripts/run-portability.sh` — Braam's twenty-one programs against
    coreutils on a fixture tree, and five of them against their Rust twins
    (T49). It needs `/dev/koru`, so it is a VM boot like every other gate that
    runs a program; an earlier draft of this list had it as a `ctest` label,
    which could never have worked.
11. Steps 4 and 7 **concurrently** (T43). Both must still be correct.

The dev kernel must have KASAN, `PROVE_LOCKING` and `DEBUG_KMEMLEAK` on from day
one; they pay for themselves in the first week.
