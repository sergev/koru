# koru — remaining tasks

This file lists only work that is still to do. The design, the ABI invariants,
the accepted gaps and everything T0–T10 established are in
[Notes.md](Notes.md); read that first.

T0–T10 are done and have been removed from this list. Every opcode except
`CANCEL` works: the module registers `/dev/koru`, configures a ring with
`SETUP`, and submits `NOP`, `DELAY_NS`, `CHECKSUM`, `OPEN`, `READ` and `CLOSE`
through `ENTER`, which blocks for completions. The arena is mmap'd, slot
exclusivity is enforced by the kernel, open files are held in a generational
handle table, teardown is safe and `rmmod` is refused while anything is live.

Each task below carries a done test. A task is finished when its done test has
been run and has passed, and when the test has been shown to fail if the thing
it checks is broken. Mark it done here, move whatever it taught into
[Notes.md](Notes.md), then delete it from this file.

**[M]** mechanical · **[R]** risky/exploratory

## Phase 3 — real operations

### T11 [R] — `CANCEL`

`res = 0` when found and cancelled, `-ENOENT` for an unknown target,
`-EALREADY` when it is already running. The target always gets its own CQE per
C1; relative ordering of the two completions is unspecified.

Done test: cancel a pending `DELAY_NS` and the target gets `-ECANCELED` while
the canceller gets `0`. Cancel a running one and the canceller gets
`-EALREADY` with the target still completing. Both CQEs always arrive.

### T12 [R] — hostile-userspace fuzz

Random SQE bytes, random slot, handle, len and off, garbage reserved fields,
eight concurrent `ENTER` threads, concurrent `munmap`, random `kill -9`.

Done test: ten minutes under KASAN, lockdep and kmemleak with zero kernel
messages. **Do not skip this** — it is the only thing that validates the TOCTOU
and validation rules.

## Phase 4 — Rust userspace

### T13 [M] — `koru-sys` crate

`#[repr(C)]` ABI structs, ioctl wrappers, `Ring::{setup, enter, mmap}`, and a
compile-time assertion that every struct's `size_of` matches the kernel's.

Done test: the T4–T11 tests re-expressed as Rust integration tests, passing.

### T14 [R] — futures and executor

Op slab keyed by `(index, generation)`, `Future` impls, and a single-threaded
executor whose `park()` is `ENTER`. The `BufSlot` is owned by `OpState`, never
by the future.

Done test: this async block prints the file, concurrently with timers completing
out of order. **This is the deliverable.**

```rust
async {
    let h = open("/etc/hostname").await?;
    let (n, buf) = read(h, buf).await?;
    print(&buf[..n]);
    close(h).await?;
}
```

### T15 [R] — drop safety

Done test: race a `read` future against a timer and drop it mid-flight. Assert
that a `CANCEL` is submitted, that the slot is *not* back in the free pool until
the target's CQE lands, and that a later op on that index does not get `-EBUSY`.
100k iterations under ASan.

## Phase 5 — C++20 userspace

No kernel changes. This phase depends only on T12, a validated kernel, so it
*can* run in parallel with Phase 4 — but **do it after T14**. The Rust binding
shakes the ABI out; writing the second binding against a settled ABI is a test
of language-neutrality, whereas writing both at once just churns the wire format
twice.

### T16 [M] — ABI conformance

`koru_abi.h` mirroring `koru_abi.rs`, with `static_assert` on every `sizeof` and
`offsetof` and on every opcode value. Add an `abi_dump` binary to each side
emitting a canonical text dump of the whole ABI surface.

Done test: the `diff` of the two dumps is empty, and deliberately perturbing one
field in either file makes the test fail. Verify that, or the test proves
nothing.

### T17 [M] — `libkoru` synchronous core

RAII `Ring` covering open, `SETUP`, `mmap` and close. `BufPool`, move-only
`BufSlot` with deleted copy operations, raw `submit()` and `reap()`,
`result<T>`. No coroutines yet.

Done test: the entire T4–T11 test matrix re-expressed in C++ and passing, the
same assertions as T13 in a different language. Any divergence here is an ABI
ambiguity worth fixing before coroutines hide it.

### T18 [R] — op slab and awaiter

An `(index, generation)`-keyed slab of `op_state`, plus `await_ready`,
`await_suspend` and `await_resume`. `~op_awaiter` marks the entry `Abandoned`,
moves the `BufSlot` into it, and submits `CANCEL`.

Done test: destroy a coroutine frame with an op in flight. Under ASan there is
no resume of a freed handle, the slot is not recycled until the CQE lands, and a
later op on that index does not get `-EBUSY`.

### T19 [R] — `task<T>`

Lazy `initial_suspend`, **symmetric transfer** on `final_suspend`, move-only,
`unhandled_exception` going to `std::terminate`. Plus `sync_wait(task<T>)`.

Done test: 100,000 nested `co_await`s complete with stack usage *measured* flat,
not assumed. This is the symmetric-transfer regression test and it silently
passes if you write it wrong and only try ten levels. A task destroyed without
being awaited must leak nothing under LSan.

### T20 [R] — C++ executor

Ready queue, a `run()` whose park is `ENTER(min_complete=1, timeout)`, and a CQE
path of slab lookup then resume or discard. Close the same empty-ring deadlock
foot-gun as the Rust side.

Done test: **the C++ demo.** `co_await open("/etc/hostname")`, then read, print
and close, concurrent with `delay` ops completing out of order. Output must be
byte-identical to the Rust demo.

### T21 [R] — C++ drop safety and cross-language interop

Done test: a mirror of T15, racing a read against a timer and destroying the
frame mid-flight, 100k iterations under ASan and UBSan. Then run the Rust and
C++ demos **concurrently** against the same module, each with its own ring, both
producing correct output. That is the language-neutrality claim actually tested
rather than asserted.

## Phase 6 — only if justified

### T22 [R] — shared mmap'd SQ/CQ

`Atomic<u32>` indices behind a `features` bit, keeping the ioctl path as a
fallback. **Requires first solving research finding 2** in [Notes.md](Notes.md):
obtaining a legitimate stable kernel virtual address into a `Page`, which may
mean patching `rust/kernel/page.rs`. Re-examine whether it is worth it at all —
with one `ENTER` per submit, the only win is syscall-free CQE reading.

Done test: the whole T4–T12 suite passes in both modes, fuzz included, in
**both** language bindings.

### T23 [R] — eventfd and tokio bridge

eventfd registration plus a tokio `AsyncFd` bridge. Only if tokio integration
becomes a goal, and try the pure-userspace waiter thread first. Note this is the
point at which the "`await_suspend` must not touch `this`" rule stops being
theoretical.

## Verification

Every task above gates on its own done test. The overall end-to-end sequence,
once everything lands:

1. `make -C <kernel-tree> M=$PWD LLVM=1 && insmod koru.ko`
2. `cargo test -p koru-sys` — ABI round-trip and per-opcode integration tests
   (T13).
3. `cargo run --example read_file` — the T14 demo: opens and reads a real file
   through coroutines while timers complete out of order.
4. `cargo run --example fuzz --release` for ten minutes (T12), with `dmesg -w`
   in another terminal. Zero kernel messages is the pass condition.
5. `cmake -B build -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`, then
   `cmake --build build`.
6. `diff <(./build/abi_dump) <(cargo run -q --bin abi_dump)` — empty (T16).
7. `ctest --test-dir build` — the C++ test matrix (T17), abandonment path
   (T18), symmetric transfer (T19) and drop-safety loop (T21), all under ASan
   and UBSan.
8. `./build/examples/read_file` — the C++20 demo (T20). Output must match step
   3 byte for byte.
9. Run steps 3 and 8 **concurrently** (T21). Both must still be correct.
10. `echo scan > /sys/kernel/debug/kmemleak; cat /sys/kernel/debug/kmemleak` —
    empty. Sleep past kmemleak's five-second minimum age first, or the scan is
    meaningless.
11. The unprivileged-user creds test from T9 — must fail `-EACCES`.
12. `rmmod koru` cleanly at the end.

The dev kernel must have KASAN, `PROVE_LOCKING` and `DEBUG_KMEMLEAK` on from day
one; they pay for themselves in the first week.
