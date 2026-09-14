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

## Phase 10 — the C++ binding

The ABI is settled, and so is the surface design. This phase transcribes it,
which is what makes the language-neutrality claim a test rather than an
assertion.

### T40 [R] — op slab and awaiter

An `(index, generation)`-keyed slab of `op_state`, plus `await_ready`,
`await_suspend` and `await_resume`. `~op_awaiter` marks the entry `Abandoned`,
moves the `BufSlot` into it, and submits `CANCEL`.

Done test: destroy a coroutine frame with an op in flight. Under ASan there is
no resume of a freed handle, the slot is not recycled until the CQE lands, and a
later op on that index does not get `-EBUSY`.

### T41 [R] — `task<T>`

Lazy `initial_suspend`, **symmetric transfer** on `final_suspend`, move-only,
`unhandled_exception` going to `std::terminate`. Plus `sync_wait(task<T>)`, and
`get_return_object_on_allocation_failure` so a frame-allocation failure yields a
null `task` rather than undefined behaviour — one static member function, and
what makes Braam's pervasive `if (task<T> t = ...)` idiom compile.

Done test: 100,000 nested `co_await`s complete with stack usage *measured* flat,
not assumed. This is the symmetric-transfer regression test and it silently
passes if you write it wrong and only try ten levels. A task destroyed without
being awaited must leak nothing under LSan.

### T42 [R] — C++ executor

Ready queue, a `run()` whose park is `ENTER(min_complete=1, timeout)`, and a CQE
path of slab lookup then resume or discard. Close the same empty-ring deadlock
foot-gun as the Rust side.

Done test: T15's demo in C++, concurrent with delays completing out of order.
Output byte-identical to the Rust demo.

### T43 [R] — C++ drop safety

Done test: a mirror of T16 — race a read against a timer, destroy the frame
mid-flight, 100k iterations under ASan and UBSan.

## Phase 11 — the C++ surface

### T44 [M] — the vocabulary and runtime entry, C++

`Error`, `result<T>`, `TRY`, `TRY_VOID`, `CO_TRY`, `CO_TRY_VOID`. Those macros
are statement expressions and need `-std=gnu++20`, where Notes.md says `c++20`;
change it there. Aliases for `Str`, `Span`, `String`, `Option`, and
`koru/braam.hpp` hoisting everything to global scope. Then the ambient ring,
`koru_main(Args) -> task<i32>`, the spawn and the at-exit hook.

Done test: the errno table from T20, in C++, giving identical results. Then a
source file written in Braam style, with no namespace qualification anywhere,
compiling with only `braam.hpp` included — and Braam's hello world running with
the same output as T21's.

### T45 [M] — the operation layer, C++

T30's function list, same signatures.

Done test: T30's signature-conformance and behavioural matrix, in C++.

### T46 [M] — buffered `File` and the iterators, C++

T31's surface. `~File` neither flushes nor closes, which is Braam's documented
behaviour and not an oversight.

Done test: T31's, including the measured `ENTER` count.

### T47 [M] — the program shell, C++

T32's surface.

Done test: T32's, against the same vectors.

### T48 [M] — the screen client, C++

T37's surface transcribed, over a C++ port of the same pure library.
`~ProcScreen` neither releases the claims nor closes the connection, because a
destructor cannot await — T38's EOF teardown is what makes that safe, and it is
the argument Braam's own header already makes.

Done test: T37's matrix in C++ against the same fake daemon, identical
assertions in a different language. Then Braam's `less` through the C++ binding
producing a pixel snapshot **byte-identical** to the Rust run of T38. That is
the language-neutrality claim made on a surface that paints rather than one
that prints, and it is a stronger test than two demos emitting the same text.

## Phase 12 — the proof

### T49 [R] — the portability proof

The deliverable for the whole Braam effort. Take real programs from Braam's
`src/cmd` and compile them against koru with the include line as the only edit.
By ascending difficulty: `echo`, `basename`, `dirname`, `pwd`, `seq`, `sleep`,
`touch`, `mkdir`, `rm`, `ln`, `truncate`, `date`, `cat`, `wc`, `head`, `tail`,
`tr`, `cut`, `uniq`, `cmp`, `tee`, `grep`. Then `less` and `edit`, which are the
full-screen half of `src/cmd` and the only two that exercise Phase 9's pure
library end to end; `less` already ran at T38, so `edit` is what this adds.

Done test: a stated number of them compile with no source change beyond the
include, and each produces byte-identical output to its coreutils equivalent on
a fixture tree, under ASan and UBSan. Any program needing a source change is
either a scope boundary already declared above or a gap in the surface, and this
task records which rather than quietly patching the source. Then the Rust half:
a handful of the same programs written as idiomatic Rust against the same
function names, producing the same bytes. That is language-neutrality tested on
a real surface rather than on one demo.

## Phase 13 — only if justified

### T50 [R] — shared mmap'd SQ/CQ

`Atomic<u32>` indices behind a `features` bit, keeping the ioctl path as a
fallback. **Requires first solving research finding 2** in [Notes.md](Notes.md):
a legitimate stable kernel virtual address into a `Page`, which may mean
patching `rust/kernel/page.rs`. Re-examine whether it is worth it at all — with
one `ENTER` per submit, the only win is syscall-free CQE reading, and Phases 5
and 7 widened the surface this must prove in both modes without widening that
benefit.

Done test: the whole T4–T12 suite plus every kernel task above passes in both
modes, fuzz included, in **both** bindings.

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
5. `cmake -B build -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`, then
   `cmake --build build`.
6. `scripts/abi.sh` — the two ABI dumps agree (T14). The screen protocol's two
   dumps share the same runner (T33).
7. `ctest --test-dir build` — the C++ matrix (T39), abandonment (T40),
   symmetric transfer (T41), drop safety (T43), the C++ surface (T44–T47) and
   the C++ screen client (T48), under ASan and UBSan.
8. `./build/examples/read_file` — the C++ demo (T42). Must match the Rust
   example of step 4 byte for byte.
9. `ctest --test-dir build -L screen` — the terminal model against Braam's own
   cell-exact tests (T34), the protocol server's rejection matrix through
   `feed()` (T36) and the five pixel oracles (T35). **These need no VM, no
   `/dev/koru` and no display server**, so they are the fastest feedback in the
   whole list and should run first in practice.
10. `ctest --test-dir build -L portability` — Braam's programs against
    coreutils, `less` and `edit` included (T49).
11. Steps 4 and 8 **concurrently** (T43). Both must still be correct.

The dev kernel must have KASAN, `PROVE_LOCKING` and `DEBUG_KMEMLEAK` on from day
one; they pay for themselves in the first week.
