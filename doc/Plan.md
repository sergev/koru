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

## Phase 8 — the Rust surface

### T31 [M] — buffered `File` and the iterators, Rust

`File` with `open`, `of`, the standard-stream accessors, `get`, `unget`, `read`,
`getline`, `put`, `write`, the `scan_*` family, `flush`, `seek`, `close`,
`detach`, and the sticky-error accessors. `Input`, `LineReader`, `TreeWalk`. The
buffering modes, and the documented promise that dropping a `File` neither
flushes nor closes, because a destructor cannot await.

This is where Braam's awaiter-with-a-fast-path shape earns its keep, and it
earns more here than on Braam, because a miss costs an `ENTER` syscall rather
than a scheduler step. The poll returns ready whenever the buffer already holds
the answer; only a genuine refill suspends.

Done test: the fast path is **measured**, not assumed. Read a large file one
character at a time and assert the `ENTER` count is proportional to refills
rather than to characters — instrument the ring and assert an exact count,
because a version that suspends every time still produces correct output and
would pass any behavioural test. Then the sticky-error idiom: a read error
mid-stream leaves `failed()` true and `err()` exact, and `Error::Cancelled` maps
to exit status 130.

### T32 [M] — the program shell, Rust

`Args` with `size`, indexing, `name` and `tail`. `Opts`, `Opt` and `OptParse`
with its synchronous, allocation-free `next`, plus `help_asked`. `usage_asked`
and `usage_error` with their fixed exit statuses of 0 and 2. `Civil` and the
calendar conversions with their month and day name tables.

Done test: `OptParse` against Braam's own option-parsing vectors — clustered
flags, an attached value, a detached value, a bare separator, a missing
argument; the calendar conversions round-tripped across a span of dates
including leap years and the epoch; and the two usage helpers writing to the
right stream with the right status.

## Phase 9 — the screen

Braam programs always have a screen: a cell grid with no control characters,
where `^C` is `'c'` with `MOD_CTRL` and colours are struct fields. Braam's
kernel owns the terminal; koru has no kernel terminal, so a daemon does.

`koru-screen` owns an SDL3 window and is a full terminal emulator — the
alternate grid, a scrolling screen, an ANSI parser, scrollback and scrolling
regions. Clients reach it over a Unix socket they `ADOPT_FD` into the ring, so
every blit and every key travels through `WRITE`, `READ` and `POLL_ADD`. **No
new kernel opcodes.** The runtime spawns the daemon on first use if it is not
already there, and one window is shared by every koru program in the session.

The program-visible API does not change: `ProcScreen` keeps its six methods,
and `Grid`, `Pane`, `TextBuf` and `TextView` are pure library that ports
directly. The transport is invisible above `proc/screen.h`, which is what makes
this a compatible replacement rather than a lookalike.

Two connections per client, not one. **Control** carries the framed protocol;
**bytes** is a raw ANSI stream whose far end is the parser, and it is what
stdout is — so `write_all` stays a plain `WRITE` to an adopted handle with no
framing in the way, and the non-painting programs print into the scrolling
screen. The byte channel is deliberately pty-shaped: swapping it for a real pty
slave later is invisible to the client. A pty *now* would force keys back into
bytes through a line discipline, which is the premise the cell model exists to
reject, and would drag in the job control this plan puts out of scope.

The rule that keeps the client tidy: **the handshake is synchronous POSIX,
everything after it is koru.** That confines ordinary syscalls to a bounded
preamble, which is where the auto-spawn retry loop has to live anyway.

Placement. The client needs `POLL_ADD`, because the socket is non-blocking and
a read with no data gives `-EAGAIN`; it needs T30's operation layer, because
the deliverable is `less` and `less` reads a file. The daemon tasks have **zero
koru dependencies** and build and test on the host with no VM, so they can be
started at any time in parallel — the linear numbering does not forbid it.

Three extensions are deliberately deferred past this list, and the design above
is shaped so each is additive rather than a rewrite. **Multiplexing** — several
clients, foreground switching, and `attach` to a second window — is what the
reserved `seq = 0` and the reserved `KS_OP_TERM_OPEN` leave room for. **A real
pty** replacing the byte channel, which the client cannot distinguish, and
which is what would let a shell run in the window. **Reconnecting** to a
restarted daemon, which a full-screen program would never notice, because the
resize path already marks the whole grid damaged. Add them here as tasks when
one of them is actually wanted.

### T33 [M] — the screen protocol ABI

`screen/ks_abi.h`, canonical because the daemon is the server and the
server owns the protocol. A 16-byte header of `len`, `op`, `flags`, `seq` and
`res`, the op table, per-op flag masks, and the payload structs. Mirrored by a
Rust module, with a dump on each side sharing T14's runner. koru's discipline
throughout: no padding, reserved fields must be zero, unknown flag bits
rejected, exact errnos.

`len` is the whole frame and must be a multiple of 8. That costs nothing and
makes a blit's cells 8-aligned, so the daemon reads them in place rather than
copying to align them.

`seq` is client-chosen and echoed, and it is **a different space from koru's
`user_data`** — conflating them is the first mistake a second implementer will
make. koru's cookie names the pump's `READ`, which completes when bytes arrive;
one such completion can carry three whole replies or half of one. Say so in the
header. `seq = 0` is reserved for unsolicited frames, which is what keeps the
deferred multiplexing additive. `KS_OP_TERM_OPEN` is defined and answers
`-ENOSYS` until then.

Done test: the `diff` of the two dumps is empty and perturbing one field in
either makes it fail — verify that, or the test proves nothing. Then three
assertions that are not about size: a cell is 8 bytes, keeping Braam's
assertion text; the header plus the blit header is a multiple of 8, which must
fire if the padding word is removed; and a maximum-width row of cells fits a
page, which is what proves T37's banding can never get stuck.

### T34 [R] — the terminal model, headless

Port Braam's `screen.cpp` and `ansi.cpp`, about 1,380 lines: the grid, the
damage rectangle, scrollback, the view, the scrolling region and the parser.
The browser-canvas descriptor and its magic number do not port — the daemon
owns its grid directly and there is no descriptor to hand anybody. No SDL, no
socket, no koru; this builds and runs on the host with nothing installed.

Done test: Braam's own `test_screen.cpp` and `test_ansi.cpp`, 722 lines of
cell-exact assertions, compiled against the port and passing unchanged but for
a harness shim, under ASan and UBSan. Then falsifiability: break the scrollback
push and only the view tests fail; break the deferred wrap, where the cursor
column may equal the width, and only the autowrap tests fail. Then a libFuzzer
target on the parser whose oracle is that after every write the cursor, the
damage rectangle and the region margins are all inside the grid — reintroduce
an off-by-one in the region and it must trip in under a second.

### T35 [R] — the renderer, the window and the pixel oracle

SDL3 window, an embedded bitmap font, cells drawn from the grid with only the
damage repainted, `SDL_Event` keys normalised to `{code, mods}`, and a window
resize driving `screen_resize`. Under a test environment variable, a
`SDL_RenderReadPixels` snapshot to a named path.

Verify in the **first hour** that `SDL_VIDEODRIVER=offscreen` plus the software
renderer initialises in the virtme-ng guest with no GPU. The fallback is a
null-renderer headless mode costing about thirty lines, and it loses only the
pixel oracles, which already run on the host.

Done test: five font-independent oracles under the offscreen driver — golden
images break on every font change and nobody regenerates them honestly.
*Geometry*, the surface is exactly the cell size times the grid. *Ink*, a
glyph's cell holds a non-background pixel and a blank cell holds none, which is
the only one that catches nothing drawing at all. *Isolation*, writing a
different character at one cell changes pixels inside it and none outside.
*Colour*, a red-background cell's modal pixel is the palette's red. *Damage*, a
blit in the middle leaves every pixel outside it byte-identical to the previous
frame, which is what separates a renderer that repaints the damage from one
that repaints everything. Each shown to fail: offset the cell origin and
isolation and damage both fire; swap `fg` and `bg` and colour fires; repaint
everything and damage fires; draw no glyphs and ink fires. Then the premise
itself: a synthesised ctrl-C arrives as `{'c', MOD_CTRL}`, never as byte `0x03`.

### T36 [R] — the daemon's protocol server

The listening socket with bind-then-rename, one connection, and the frame
handler written as a **pure `feed()` with no I/O in it** — the event loop does
`recv`, `feed`, `drain_replies` and nothing else. Both claims as RAII objects
released on EOF, with the screen restore Braam's `~FullScreen` does. Echo is
implemented rather than stubbed: koru has no shell to call it, but `feed()` is
pure, so a test can drive it directly and it can be made falsifiable without a
caller.

Two rules, one borrowed and one that cannot be. **Every accepted request frame
produces exactly one reply**, which is C1 transposed and is what keeps the
client's `seq` map from leaking a coroutine that never resumes. But E1 splits:
a frame that parses and is then rejected on content gets an exact errno and the
connection continues, while a frame that does not parse has desynchronised the
stream with no way to find the next boundary, so the daemon sends one `-EPROTO`
and closes. An ioctl has a private snapshot; a byte stream does not.

Resize needs care. Braam's kernel cannot invent a reply to a parked key read,
so it signals; the daemon owns the reply, so it answers a parked `KEY_READ`
with `-EINTR` **and a full payload** carrying the new geometry. `next_key`
resizes from what it already has, so the signature and semantics are unchanged
and source compatibility is exact. One new rule to assert: `-EINTR` is the only
negative `res` that carries a payload. A blit racing a resize must not report a
spurious `-EINVAL`, so the blit **carries the geometry the client believed**:
matching geometry with an out-of-range rectangle is a real bug and gets
`-EINVAL`, differing geometry is stale, so draw nothing and reply success with
a stale flag.

Done test: the rejection matrix driven through `feed()` with no socket —
unknown op, unknown flag bit, non-zero reserved, `len` below the header, above
the frame cap, or not a multiple of 8, a blit overflowing its declared
geometry, a stale blit, a second parked key read, a blit without the claim —
each producing exactly one reply with the exact errno, and each shown to fail
when its own check is deleted. Framing: one frame split across three `feed()`
calls and two frames in one both produce the same replies as a whole frame, and
EOF mid-frame is a clean teardown rather than a protocol error, shown to fail
by making a partial frame `-EPROTO`. The reply-count invariant asserted after
every case. The claim lifetime: take the screen, blit, drop the connection,
assert the saved cells are back, then delete the restore and watch only that
assertion fail. Then a libFuzzer target on `feed()` under ASan whose oracle is
that invariant plus T34's grid invariants — remove the `len` bound and it must
give a heap overflow read immediately.

### T37 [R] — the screen client, Rust

`ProcScreen`'s six methods unchanged over the ported pure library: `Grid`,
`Pane`, `TextBuf`, `TextView`, plus `pack_blit`, which is pure and therefore
testable with no socket. A synchronous POSIX preamble, then a pump coroutine
reading into its own slot and demultiplexing by `seq`, a single-writer send
path, and the banding loop. Stdout is the byte channel's handle.

The grid is ordinary heap and **cannot** be an arena slot: a slot with an op in
flight is exclusively owned by the kernel, so a grid living in one would be
unpaintable for the duration of every blit. Pack the damage straight into a
slot instead. Be honest in Notes about the cost — grid to slot, slot to kernel
bounce, bounce to socket — where a plain `write(2)` would skip the first two.
**This is the first place in the project where the central invariant has a
measurable price**, and every earlier op's price was zero. It is bounded: an
80x24 full repaint is 15 KB.

A maximum blit does not fit a slot — 512x256 cells is exactly 1 MiB and the
headers push it over — so `flush` bands the damage into as many frames as fit.
Do not raise the cap: banding is provably unstuck, because a maximum-width row
is 4096 bytes and `slot_size` is a multiple of `PAGE_SIZE`, so a row always
fits a page.

**The ordering hazard is the most likely bug here.** `WRITE` is deferred to a
workqueue and koru has no op-linking, so two concurrent writes on one handle
have no ordering, which on a stream socket braids two frames into permanent
corruption. The connection enforces exactly one write in flight and queues
senders behind it. That is a userspace serialisation the ABI does not provide
and should not.

Done test: against a fake daemon the test itself speaks, over a `socketpair`,
one end `ADOPT_FD`'d by the real client on a real ring in the VM. Out-of-order
replies: park a key read, blit, answer the blit first, and `flush()` must
return while the key read is still parked. The single-writer rule: two
coroutines blitting concurrently produce two whole frames in some order and
never interleaved bytes — delete the serialisation and the test must see a
frame whose `op` is not a known opcode. Banding: a full 512x256 repaint with a
64 KiB slot produces frames whose rectangles tile the damage exactly once, no
cell twice and none missed. Backpressure: the fake daemon stops reading until
the socket buffer fills, and the client must make progress through `POLL_ADD`
rather than spin, asserted as a bounded `ENTER` count — which is also the first
exercise of T22's softirq wake path on a socket rather than a FIFO. Failure:
close the fake end mid-frame; every parked `seq` completes `-ECONNRESET`, every
later call returns it without touching the wire, and nothing hangs, so arm the
alarm and keep stdout line-buffered.

### T38 [M] — auto-spawn, lifecycle and the end-to-end run

Socket path from `KORU_SCREEN_SOCK`, else under `XDG_RUNTIME_DIR`, refusing a
directory that is not ours and not `0700`. Spawn guarded by an exclusive
`flock` beside the socket: connect, take the lock, connect again in case the
winner finished while we waited, and **only the lock holder may unlink** a
socket whose daemon is dead — which is what the lock is really for. The daemon
binds a temporary name and renames it into place, so a half-initialised socket
is never connectable. It persists after its last client: a window that vanishes
between two commands is not a terminal.

Claims are released on socket EOF, which is **better than Braam's**, because
there is no process record to leak — the claim's lifetime is the socket's and
the kernel guarantees the socket dies. It holds on `SIGKILL`, on `_exit`, on a
panic. That is the strongest argument for a connected stream socket.

Then the deliverable: Braam's `less`, 174 lines, compiled against koru with the
include line as the only change.

Done test: the spawn race, twenty processes started at once against no daemon —
exactly one daemon exists afterwards, counted by both the listening inode and
the process table, and all twenty connect; delete the `flock` and more than one
must appear. A stale socket is unlinked and replaced, and a *live* daemon's
socket is never unlinked, asserted by racing twenty clients against a running
daemon and checking its pid is unchanged. Daemon death: `SIGKILL` it while a
client holds the screen, and the client's next call returns `-ECONNRESET` and
the program exits non-zero rather than hanging. Client death: `SIGKILL` a
client mid-blit, and the daemon restores the saved screen, serves the next
client, and logs no protocol error. Then `less` itself in the VM under the
offscreen driver, painting a fixture file, taking `j`, `G` and `q`, and exiting
— with T35's isolation and damage oracles asserting the painted result, so that
"it ran" and "it drew the right thing" stay two separate claims.

## Phase 10 — the C++ binding

The ABI is settled, and so is the surface design. This phase transcribes it,
which is what makes the language-neutrality claim a test rather than an
assertion.

### T39 [M] — `libkoru` synchronous core

RAII `Ring` covering open, `SETUP`, `mmap` and close. `BufPool`, move-only
`BufSlot` with deleted copy operations, raw `submit()` and `reap()`, and
`result<T>` carrying both the Braam `Error` and the raw errno. No coroutines.

Done test: the T4–T11 matrix re-expressed in C++, mirroring the test list in
`rust/sys/tests/kernel.rs` case for case. Any divergence is an ABI
ambiguity worth fixing before coroutines hide it.

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
