# koru — remaining tasks

This file lists only work that is still to do. The design, the ABI invariants,
the accepted gaps and everything T0–T12 established are in
[Notes.md](Notes.md); read that first.

T0–T12 are done and have been removed from this list. Every opcode listed there
is implemented, and the whole validation surface has been fuzzed for ten minutes
under KASAN, lockdep and kmemleak with zero kernel messages. The arena is
mmap'd, slot exclusivity is enforced by the kernel, open files are held in a
generational handle table, a queued op can be genuinely dequeued, and closing
the fd cancels whatever is still queued.

Each task below carries a done test. A task is finished when its done test has
been run and has passed, and when the test has been shown to fail if the thing
it checks is broken. Move whatever it taught into [Notes.md](Notes.md), then
delete it from this file.

**[M]** mechanical · **[R]** risky/exploratory

## The userspace API is Braam's

`/home/vak/Project/Braam/braam-core` is a browser-hosted operating system whose
programs are wasm modules with no libc, no threads and no stack switching. Its
syscalls are a submit import plus a completion export, and every blocking
operation is a C++20 coroutine. Different substrate, the same structural bet
koru makes: submission and completion are separate events.

So koru's userspace API is Braam's process-side API — the surface documented in
that project's `doc/Programming_Manual.md` — minus what this substrate cannot
carry. That buys a surface already proven to carry real work, and it turns the
language-neutrality claim into something much harder to fake than two demos
printing the same bytes: Braam's own programs become the conformance suite.

Most of it maps almost exactly. Braam's `Task<T>` is lazy, move-only and uses
symmetric transfer on `final_suspend`, which is what T19 already specified
independently. `Result<T, Error>` with `TRY`/`CO_TRY` is what Notes.md already
proposes as `koru::result<T>`, and it is native Rust. The function names are
already snake_case and already idiomatic in both languages. Two details map
*better* here than on Braam: Rust's `poll` returning `Ready` is natively the
awaiter-with-a-fast-path shape Braam has to simulate, and that fast path earns
more on koru, because a miss costs an `ENTER` syscall rather than a scheduler
step.

Two observations worth keeping. Braam's `write_all(fd, Str s)` borrows the
caller's buffer across the suspension, which on raw io_uring is unsound in Rust
and is the whole reason owned-buffer runtimes exist. On koru it is safe: the
bytes are copied into an arena slot before the op is submitted, so the borrow
ends before the await begins. The slot indirection buys back a borrowed-buffer
API. And Braam's `read_chunk` returns an owned string rather than filling a
caller buffer, for an unrelated reason — a reply is only valid until the next
syscall on that slot — which is exactly the cancel-safe shape a completion ring
wants. Two designs, the same answer, different pressures.

What does not map stays out: the terminal is a cell grid with no control
characters underneath, the host services are browser APIs, and processes,
pipes, signals and job control need opcodes koru does not have and a model it
does not want. Braam's freestanding constraints — no libc, no namespaces, the
512-byte coroutine frame rule — are wasm artefacts and do not apply. koru is a
superset on cancellation: Braam has none in-process, while koru has `CANCEL`,
drop-based abandonment and the generational slab.

One thing does not map at all, and it reopens the kernel. Braam's hello world
is `co_await write_all(SYS_STDOUT, "Hello, ")`. koru has no `WRITE` opcode, and
`OPEN` refuses anything but regular files and directories on purpose, because a
blocking read in a kworker cannot be interrupted and would pin a workqueue
thread permanently. Stdout cannot reach the ring today. Phase 6 closes that,
and it is scoped against the actual 7.1.12 source tree rather than against what
the obvious approach would suggest.

## Reading this list

Task numbers are permanent. T13 through T23 are cross-referenced from
[README.md](../README.md), [CLAUDE.md](../CLAUDE.md), [Notes.md](Notes.md) and
the C test sources, so none of them is renumbered. T22 and T23 keep their
numbers but now sit last, because "only if justified" belongs at the end.

The Rust crates are a Cargo workspace under `user/`: `koru-sys` holds the ABI
structs, the ioctl wrappers and `abi_dump`; `koru` holds the futures, the
executor and the Braam surface. Notes.md currently spells this three different
ways; this is the spelling.

## Phase 4 — Rust userspace

### T13 [M] — `koru-sys` crate

`#[repr(C)]` ABI structs, ioctl wrappers, `Ring::{setup, enter, mmap}`, and a
compile-time assertion that every struct's `size_of` matches the kernel's. Also
the errno-to-`Error` mapping table, since both bindings need it and it is
ABI-adjacent.

Done test: the T4–T11 tests re-expressed as Rust integration tests, passing.

### T14 [R] — futures and executor

Op slab keyed by `(index, generation)`, `Future` impls, and a single-threaded
executor whose `park()` is `ENTER`. The `BufSlot` is owned by `OpState`, never
by the future.

Done test: this async block prints the file, concurrently with timers
completing out of order. **This is the deliverable.**

```rust
async {
    let h = open_at("/etc/hostname", KORU_O_RDONLY).await?;
    let text = read_chunk(h).await?;
    write_all(STDOUT, &text).await?;
    close_fd(h).await;
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
emitting a canonical text dump of the whole ABI surface. The current
`test/koru_abi.h` mirrors the structs but none of the caps, the flag masks or
`KORU_CQE_F_MORE`; add those on both sides now, before Phase 6 starts adding
more.

Done test: the `diff` of the two dumps is empty, and deliberately perturbing one
field in either file makes the test fail. Verify that, or the test proves
nothing.

### T17 [M] — `libkoru` synchronous core

RAII `Ring` covering open, `SETUP`, `mmap` and close. `BufPool`, move-only
`BufSlot` with deleted copy operations, raw `submit()` and `reap()`,
`result<T>`. No coroutines yet. `result<T>` carries both the Braam `Error` and
the raw errno it came from, so the fifteen-value mapping loses nothing.

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
`unhandled_exception` going to `std::terminate`. Plus `sync_wait(task<T>)`, and
`get_return_object_on_allocation_failure` so a frame-allocation failure yields a
null `task` rather than undefined behaviour — one static member function, and
what makes Braam's pervasive `if (task<T> t = ...)` idiom compile.

Done test: 100,000 nested `co_await`s complete with stack usage *measured* flat,
not assumed. This is the symmetric-transfer regression test and it silently
passes if you write it wrong and only try ten levels. A task destroyed without
being awaited must leak nothing under LSan.

### T20 [R] — C++ executor

Ready queue, a `run()` whose park is `ENTER(min_complete=1, timeout)`, and a CQE
path of slab lookup then resume or discard. Close the same empty-ring deadlock
foot-gun as the Rust side.

Done test: **the C++ demo.** The T14 program written a second time, in C++,
concurrent with `delay` ops completing out of order. Output must be
byte-identical to the Rust demo.

### T21 [R] — C++ drop safety and cross-language interop

Done test: a mirror of T15, racing a read against a timer and destroying the
frame mid-flight, 100k iterations under ASan and UBSan. Then run the Rust and
C++ demos **concurrently** against the same module, each with its own ring, both
producing correct output. That is the language-neutrality claim actually tested
rather than asserted.

## Phase 6 — kernel work the Braam surface requires

The kernel reopens, for a reason stated above: the target API cannot write to a
terminal or a pipe at all.

One finding governs the whole phase. **`override_creds` is not exported.**
`prepare_creds` and `abort_creds` are, but neither can install a cred set, so
there is no mechanism by which an out-of-tree module can defer a creds-sensitive
operation on this kernel. Every path-walking op below is inline, permanently.
Notes' finding 3 is not a "for now".

Three obligations apply to every task in this phase rather than being repeated
in each:

- `KoruParams::features` gains a bit per capability, so userspace probes rather
  than submitting and watching for `-EINVAL`. This is what `features` was
  reserved for.
- The fuzzer grows with each opcode: a per-opcode allowed-`res` set, a
  per-opcode `extra` oracle replacing the current blanket `extra == 0`
  assertion, and — for anything touching the filesystem — a per-run temporary
  directory plus a path generator structurally incapable of emitting `..` or an
  absolute path. That sandboxing is a property of the test, not the kernel, and
  it is the most important safety property in the phase. `unlink` under a root
  fuzzer pointed at `/` is not a thought experiment.
- Any new `#[repr(C)]` is mirrored in `koru_abi.h` and covered by T16's dump and
  diff, or the newest and least-reviewed structs become the only unverified
  ones.

Nothing here bumps `KORU_ABI_VERSION`. New opcode numbers are additive under the
existing "unimplemented opcodes complete with `-EINVAL`" rule. A new bit inside
an existing `*_FLAGS_ALL` mask only relaxes a rejection, so it is safe in both
directions: an old binary never sets it, and a new binary on an old kernel gets
`-EINVAL` as a completion. Per-opcode meanings for `Cqe::extra` are within its
documented contract, and new data-plane structs change no existing size or
offset.

### T24 [M] — `KORU_OP_WRITE`

`len` bytes from slot offset 0, written at file offset `off` of `handle`.
Deferred to the workqueue, mirroring `READ`. New `check_writable` guarding
`FMODE_WRITE | FMODE_CAN_WRITE`, regular files only for now, and the same
`f_op->write` / `write_iter` shape check that keeps `not supported for file` out
of the log. Use `kernel_write`, not `__kernel_write`, which skips
`rw_verify_area` and freeze protection and calls itself `EXPORT_SYMBOL_DONTUSE`
in a comment. Chunk page by page rather than reusing `read_slot`, which would
allocate a whole megabyte. Take the arena mutex only around each `read_raw`,
never across `kernel_write`.

`O_APPEND` is the non-obvious one. `kernel_write` copies `f_iocb_flags`, which
carries `IOCB_APPEND`; koru's own `open_flags` has no `O_APPEND`, but an adopted
handle (T27) can have it, and then `off` is silently ignored and the write lands
at EOF. Document it in `koru_abi.rs`.

While here: `off` now means a file offset on `READ` and `WRITE`, a within-slot
offset on `OPEN` and `CHECKSUM`, nanoseconds on `DELAY_NS` and a target cookie
on `CANCEL`, and this phase adds four more meanings. The per-opcode doc comments
in `koru_abi.rs` are the only defence a second implementer has, so make that
list explicit there.

Done test: a known pattern written to a fresh temp file and read back with
`pread(2)`; placement at a non-zero `off` into a sparse file; the rejection
matrix — read-only handle to `-EBADF`, directory handle, zero `len`, oversized
`len`, out-of-range `slot`, non-zero `rsvd0`, unknown flag bit — each shown to
fail when its kernel check is deleted; two concurrent writes to one slot giving
one `-EBUSY`; and the regression test that matters, cancelling a queued `WRITE`
and asserting the slot is freed, which fails if `KORU_OP_WRITE` is missing from
`OpWork::held_slot`. That omission is the single most likely bug in this task
and it manifests as every later op on that index getting `-EBUSY` with nothing
saying why. Heavy phase.

### T25 [M] — `KORU_O_NONBLOCK` and the non-regular-file gate

`RWF_NOWAIT` is not the mechanism. `kiocb_set_rw_flags` returns `-EOPNOTSUPP`
unless `f_mode & FMODE_NOWAIT`, and a `filp_open`'d FIFO never has that bit —
it is set only by `pipe(2)`, `sock_alloc_file`, eventfd, timerfd, signalfd and
userfaultfd. A tty never sets it, and `n_tty` checks `f_flags & O_NONBLOCK` and
nothing else. So the mechanism is `O_NONBLOCK` at open time.

Add the flag to `KORU_OPEN_FLAGS_ALL`, translate it in `open_flags`, and relax
`check_readable` and `check_writable` to admit a non-regular file when and only
when `f_flags & O_NONBLOCK` is set. **koru never sets or clears that bit on a
file it did not open**: for an adopted descriptor the `struct file` is shared
with the rest of the process, and flipping the bit on stdin changes behaviour
for everything else holding that open file description.

This also closes a denial of service Notes.md currently lists as accepted. A
blocking `filp_open` on a peerless FIFO stalls the whole ring *before* the
file-type check can ever run, and the file-type restriction does not close it.
An explicit opt-in flag does, with nothing changing for existing callers.

Done test: opening a peerless FIFO with `KORU_O_NONBLOCK` returns promptly,
which is the assertion that closes the stall; `READ` on it gives `-EAGAIN`; a
FIFO handle opened without the flag is rejected at submit time with `-EINVAL`,
and deleting that check must make the test **hang** rather than fail, so arm the
alarm and keep stdout line-buffered. The regular-file restriction was one of two
independent readability guards, so relaxing it leaves the `f_op` guard alone for
a whole class of file; re-verify that guard on its own rather than assuming it.

### T26 [R] — `KORU_OP_POLL_ADD`

Single-shot; multishot stays foreclosed by admission control. `handle` names the
file, `len` carries a requested-event mask in koru's own bit values, `off` and
`slot` must be zero, and `res` is the returned mask — always non-negative, so
there is no collision with errnos.

`vfs_poll`, `init_poll_funcptr` and `init_waitqueue_func_entry` are static
inlines with no Rust helper; each is two or three field stores, reimplemented in
Rust. The poll mask constants are hardcoded with a comment, exactly as
`FMODE_READ` already is, because bindgen cannot evaluate their casts. A file
whose `f_op->poll` is `None` completes immediately as always-ready, which is the
correct answer for a regular file. Allow exactly one `poll_wait` per arm; a
second sets a flag and the op completes `-EOPNOTSUPP`, rather than
reimplementing io_uring's double-entry machinery.

The wake callback is the risk, and it is a new kind of risk for this module. It
runs with the waitqueue head's spinlock held and, for a socket, in **softirq**.
`kernel::sync::SpinLock` is plain `spin_lock` and there is no `SpinLockIrq`
anywhere in this tree, so the callback may not call `complete()`, may not take
`pending`, may not `fput` and may not `remove_wait_queue`. All it does is win a
one-shot atomic token, stash the mask, and `enqueue`, which is IRQ-safe. The
kworker then does the removal and the completion in process context. This is a
new constraint of the same family as "`ENTER` must never touch a `UserSlice`
while holding the ring `SpinLock`" and belongs next to it in Notes.

Two consequences elsewhere, both load-bearing. `CANCEL` needs a second arm: an
armed poll is on no workqueue, so `cancel_delayed_work` returns false and the
existing code would answer `-EALREADY` for an op that is merely waiting, for
ever. The same token decides it — the winner disarms, removes and adopts exactly
one reference; the loser answers `-EALREADY`. That is the only structure that
preserves the refcount rule Notes verified by breaking in both directions. And
`cancel_all` must disarm every armed poll **inside** the `pending` lock, before
the registry is swapped out and the files are dropped. Backwards is not a leak,
it is a use-after-free walking a freed list node from softirq on a socket the
process no longer holds. The waitqueue head stays alive only because the op
holds an `ARef<File>`, which is the entire safety argument and is what makes the
drop order load-bearing.

Explicit `POLL_ADD` rather than io_uring-style auto-arm on `-EAGAIN`. Auto-arm
does not remove any of this machinery, it adds a state machine on top; it
multiplies the refcount rule across five states on the module's most delicate
code; it reopens the CQ-reservation exhaustion the fuzzer already found, since a
read on an empty FIFO would hold its reservation and its slot indefinitely; and
it hides the wait from the coroutine, so racing a FIFO read against a timer
means cancelling a read that may be mid-retry. Revisit only after multishot.

Done test: `POLL_ADD` on a regular-file handle completes immediately with the
default mask, which proves the no-`poll` path. Then the deliverable: arm on a
FIFO, assert no completion, have a helper thread write a byte, assert completion
with the read bit, then read that byte. Cancelling an armed poll gives the
target `-ECANCELED` and the cancel `0`, after which writing to the FIFO must
produce **no** CQE at all — omitting `remove_wait_queue` gives a KASAN
use-after-free under the dmesg gate, and omitting the token gives a double
completion that breaks C1 and that the fuzzer's consumed-equals-reaped assertion
sees directly. Close the ring fd with a poll armed, write to the FIFO, then
`rmmod` inside the existing release-timing gate. Run a cancel-versus-wake race
loop gated on reaching both arms. Then the deliberate breakage: call
`complete()` from the wake callback and require `PROVE_LOCKING` to report
inconsistent lock state and the dmesg gate to fail the run. Lockdep is the real
oracle for this task.

### T27 [M] — `KORU_OP_ADOPT_FD`

An opcode rather than a fourth ioctl, so it inherits C1, E1, the `user_data`
echo, batching with the `OPEN` it replaces, and the fuzzer's existing oracle for
free. An ioctl returning a handle would be a third control path with its own
error convention. `off` carries the descriptor as a `u64` that must be at most
`i32::MAX`, so `AT_FDCWD`-style magic numbers can never reach `fget`; `len`,
`slot` and `handle` must be zero. Using `off` rather than `handle` keeps "the
`handle` field is a koru handle except on `OPEN`" from acquiring a third
exception.

Inline, but for a different reason from `OPEN`: `fget` resolves against
`current->files`, which in a kworker is the kthread's table — either NULL or
init's. Use the owning `LocalFile::fget` plus `assume_no_fdget_pos`, never
`fdget`, whose light reference is valid only while the fd table cannot change;
this reference outlives the ioctl by design and a kworker on another CPU will
use it. The safety condition is that there are no active `fdget_pos` calls on
this thread, which holds because the ioctl path takes `fdget`; put that
reasoning in the comment verbatim. `O_PATH` needs no check, because `fget` is
`__fget(fd, FMODE_PATH)` and already returns NULL for them.

The security statement is the opposite of `OPEN`'s and stronger. `fget` performs
no permission check at all, so **`ADOPT_FD` grants koru no authority the
submitting task does not already hold.** Worth stating precisely because the
reflex is to assume otherwise.

Two rejections whose absence is catastrophic rather than subtle. Any koru
descriptor, not merely our own, gives `-ELOOP`: the ring's file holds
`Arc<RingCtx>` as private data and the handle table lives inside `RingCtx`, so
adopting it makes `release` unreachable, the module permanently unloadable and
the arena leaked — and two rings do the same thing in two hops, so comparing
against our own file alone is insufficient. Detect it by comparing `f_op`
against the ring's own file, which means threading the ring's `&File` down into
`dispatch`. Do that here; `ADOPT_FD` is not the last opcode to want it. And a
non-regular adopted descriptor without `O_NONBLOCK` must be refused for `READ`
and `WRITE` by T25's gate, **which is why this task is sequenced after T25**:
shipped first, the obvious `cat`-through-koru demo wedges a kworker on its first
read.

Done test: adopt descriptor 1 where stdout is a pipe under the harness, write
through the handle, parent reads the bytes. That is "stdout is usable", tested.
Own ring fd and a second ring's fd both give `-ELOOP`, and with the check
deleted `rmmod` must fail and one ring must leak — measured by the
release-timing gate and field 1 of `/proc/sys/fs/file-nr`, not by kmemleak,
since our own table still references the file. A bad, closed or out-of-range
descriptor gives `-EBADF`. The privilege mirror to `OPEN`'s test, asserting the
*opposite* outcome: a child dropped to nobody adopts a descriptor the parent
opened on a root-only file and **succeeds**; keep both in the same section so
the contrast is visible, because if this case ever starts failing, something has
begun re-checking permissions at use time. Lifetime: adopt, `close(2)` the
descriptor in userspace, read through the koru handle and still get data, which
catches an `fdget` mistake at once. Then two thousand adopt-and-close cycles
against `file-nr`, heavy phase.

### T28 [M] — `KORU_OP_STAT` by handle, and `KoruStat`

`res: i64` plus `extra: u64` is sixteen bytes and a `kstat` is about a hundred
and fifty, so the result goes in the slot — that is what the arena is for.
`slot` is the destination, `off` a within-slot byte offset, `len` the caller's
buffer size, which doubles as version negotiation: the kernel writes
`min(len, sizeof(KoruStat))` and returns that in `res`. `extra` carries
`kstat.result_mask`, the set of fields actually valid, which is the single most
useful thing to put there.

`KoruStat` is a new `#[repr(C)]` in `koru_abi.rs`, held to the same offset
assertions and `AsBytes`/`FromBytes` discipline as `Sqe` and `Cqe`: fixed 64-bit
fields, times as second-plus-nanosecond pairs, device numbers as explicit major
and minor rather than the kernel's internal `dev_t`, and a `reserved` array the
kernel zeroes so a later field is a carve-out exactly as `handle_count` was.
Translate `kstat` field by field, never by `transmute` — it is kernel-internal
and the shortcut looks fine until a kernel bump.

Deferred, because `vfs_getattr` blocks on NFS and FUSE. It holds a slot, so it
joins `held_slot` — the second instance of T24's trap.

Two things that are easy to get wrong. The kernel must zero the whole
destination region before filling it: `READ`'s "the rest of the slot is the
caller's own data, so there is nothing to scrub" argument does not transfer,
because a partially filled struct makes the caller read its own stale bytes as
if they were kernel-reported values. That is a correctness trap, not a leak. And
`kstat.uid` is a `kuid_t`, meaningful only through
`from_kuid(current_user_ns(), ...)`; in a kworker that namespace is init's, so a
deferred stat silently reports wrong numbers inside a container. Carry a
reference to the submitter's user namespace in `OpWork` and translate in the
worker — capturing the translated value at submit time is impossible, since the
stat has not happened yet. This is finding 3's quiet sibling: it produces wrong
numbers rather than a privilege escalation, so nothing catches it by accident.

Done test: every field cross-checked against `fstat(2)` on the same path. That
comparison is the whole test and it is cheap. A `len` smaller than the struct
returns `res == len` and leaves a sentinel written at `off + len` beforehand
untouched, which is the truncation contract. The rejection matrix, and two
concurrent stats on one slot giving one `-EBUSY`. `extra` equals the returned
mask. Zeroing is tested by pre-filling the slot with a poison byte and asserting
every byte of the declared struct is either a real field or zero. The uid
mapping is asserted against the caller's own view, with a comment in the test
saying plainly that this passes on the dev VM and would fail in a container if
the namespace were not carried — a test that only fails where nobody runs it is
not evidence.

This is the first opcode to set `extra`, so it is where the fuzzer's blanket
`extra == 0` oracle becomes per-opcode. Forgetting that makes the fuzz green for
the wrong reason.

### T29 [R] — `kern_path` plumbing: `TRUNCATE`, `UTIMES`, `READLINK`

Three simple path operations, chosen to land the shared infrastructure before
anything runs under a dentry lock. All inline, permanently, because
`override_creds` is not exported.

`linux/namei.h` is absent from `rust/bindings/bindings_helper.h`, so `kern_path`
and every `LOOKUP_*` are missing from `bindings::` even though they are
exported. New `kernel/koru_path.rs` holds every hand-declared
`unsafe extern "C"` prototype in one place, each with its C signature quoted
verbatim in a comment above it. Nothing checks these — there is no
`static_assert` that can help — so the file carries a written obligation to
re-check on every kernel bump, and Notes records it.

Guard types with `Drop` for `mnt_want_write`/`mnt_drop_write` and for
`path_put`. An early return through `?` that skips `mnt_drop_write` pins the
filesystem against read-only remount until reboot. This is one of the few places
Rust genuinely helps; take it.

`TRUNCATE` puts the new length in `off`, since it is a file offset and there is
no ambiguity, and requires `len` to be zero; `vfs_truncate` does its own
`mnt_want_write` and permission check. `UTIMES` reads a two-timespec struct from
the slot immediately after the path, reusing the kernel's own `UTIME_NOW` and
`UTIME_OMIT` sentinels rather than inventing anything, since `vfs_utimes` checks
them directly. `READLINK` is **not** `vfs_readlink`, whose buffer is a
`char __user *` and which would both violate the central invariant read
literally and fault; it is `kern_path` without `LOOKUP_FOLLOW` — which is the
entire point — then `vfs_get_link`, then `do_delayed_call`, which is a null
check and an indirect call and whose omission leaks a page per page-backed
symlink. NUL-terminate in the slot and return the length excluding the NUL;
there is no compatibility to keep and every caller wants it.

Done test: each of the three cross-checked against its libc equivalent —
`st_size` after truncate, `st_atim` and `st_mtim` after utimes, `readlink(2)`
against the slot contents. Then the creds test, which has never been written for
anything but `OPEN`: a child dropped to nobody truncates a root-owned file and
gets `-EACCES`, after which the op is deliberately deferred to the workqueue and
must be seen to succeed as root. That is the regression test for the whole
inline-because-of-creds rule and it is the most valuable assertion in the phase.
Then two thousand readlinks of a long symlink in the heavy phase with
`do_delayed_call` deliberately dropped, which kmemleak must report.

### T30 [M] — `KORU_OP_STATX_AT`

Stat by path: `kern_path` with `LOOKUP_FOLLOW`, then the same `vfs_getattr` and
the same `KoruStat` as T28, then `path_put`. Inline, for `OPEN`'s reasons —
`current->fs` for relative paths, `inode_permission` on every component, and the
LSM seeing the right task. Nearly free once T28 and T29 have landed.

Path handling reuses `do_open`'s recipe verbatim: clamp to `PATH_MAX`, copy
under a slot claim, reject an embedded NUL in the kernel copy, append our own.
The path goes in and the struct comes out of the same slot, held under a single
claim for the whole inline op — the path is consumed into a `KVec` before
anything is written, so there is no acquisition-order question at all. Document
that the operation overwrites the path it was given.

Done test: a known path matches `stat(2)` field for field; a path the
dropped-privilege child cannot traverse gives `-EACCES`, reusing T29's creds
harness and shown to fail when the op is deferred.

### T31 [M] — `KORU_OP_MKDIR` and `KORU_OP_SYMLINK`

The `start_creating_path` and `end_creating_path` happy path. `vfs_mkdir`
returns a possibly different dentry, and it is that one, not the one
`start_creating_path` handed over, that must go to `end_creating_path`;
confusing them unlocks the wrong inode. `mnt_idmap` is a static inline doing one
`READ_ONCE` and is reimplemented in Rust.

`MKDIR` carries its mode in `handle`, free on this opcode exactly as `OPEN`'s
flags are, masked to the permission bits with everything else rejected; the VFS
applies the umask. Note the consequence: Notes.md records that there is no
`O_CREAT` because no field can carry a creation mode, and once `MKDIR`
establishes this convention that reason is gone.

`SYMLINK` needs two paths in one slot. The encoding is two NUL-terminated
strings back to back with `len` covering both: read exactly `len` bytes, require
**exactly one** interior NUL, split there, append our own NUL to the second, and
reject either side empty. This is a different parse, not a relaxation of the
embedded-NUL rule. Factor it into one helper, since `RENAME` needs it too. Do
not use `off` as a second length — it collides with `off`'s within-slot meaning.

Pass NULL for `delegated_inode` throughout the path-mutating set.
`try_break_deleg` with NULL returns `-EWOULDBLOCK` without taking a reference,
so there is no `iput` bookkeeping; the cost is that NFS delegations surface as
`-EWOULDBLOCK` rather than being broken. Right trade for a first cut, and
documented.

Done test: both verified with `stat(2)` and `readlink(2)`; the creds case
against T29's harness; the two-path parse rejection matrix — zero interior NULs,
two, one at position zero, one at the last byte — each shown to fail when its
check is deleted. Then two thousand mkdir-and-rmdir pairs in the heavy phase
followed by an assertion that the filesystem can still be remounted read-only,
which is the only instrument that sees a leaked write count.

### T32 [R] — `KORU_OP_UNLINK` and `KORU_OP_RMDIR`

`start_removing_path` is not exported, and the only exported variant takes a
`__user` name and is therefore unusable. So the sequence is assembled by hand:
split the path at its last separator in the module, `kern_path` the parent with
`LOOKUP_FOLLOW | LOOKUP_DIRECTORY`, `mnt_want_write`, build a `qstr` whose hash
`start_removing` computes for you, `start_removing`, `vfs_unlink` or
`vfs_rmdir`, `end_dirop` — which is exported and bound, the one lucky break here
— then unwind. This hand-assembly is why these two are separated from T31.

Rejecting what `filename_parentat` would have rejected as a non-normal last
component — empty, `.`, `..`, a trailing separator — is our own job here.
Missing it is how removing a path ending in `..` does something surprising.

Done test: both verified with `access(2)`; `rmdir` on a non-empty directory
giving `-ENOTEMPTY`, `unlink` on a directory giving `-EISDIR`, `rmdir` on a
regular file giving `-ENOTDIR`; a path ending in `..` giving `-EINVAL` from our
own check, which when deleted must fail *differently* rather than not at all;
the creds case; and the write-count balance assertion from T31.

### T33 [R] — `KORU_OP_RENAME`

`start_renaming` — which calls `lookup_one_common` on both sides, so again no
hash work — then `vfs_rename` and `end_renaming`, with `mnt_want_write` on both
mounts and a cross-mount rejection of `-EXDEV` before anything starts. By far
the most intricate of the path operations, which is why it is alone and last.
Reuses T31's two-path parse. `struct renamedata` is already in `bindings::`.

Done test: rename within a directory, across directories, onto an existing file,
and across mounts for `-EXDEV`; the creds case; the write-count balance
assertion on both mounts.

### T34 [R] — `KORU_OP_READDIR` and `KoruDirent`

A `#[repr(C)]` wrapper whose first field is a `dir_context`, recovered in the
`filldir` callback with the `container_of!` macro `OpWork::delayed_work` already
uses — put the context first so the offset is zero, but use the macro anyway so
a later field reorder cannot silently break it.

Three constraints on that callback, none obvious, all forced. It runs with the
inode's `i_rwsem` held for read, because `iterate_dir` takes it before calling
`iterate_shared`, so it **must not take the arena mutex at all** — that is
Notes' lockdep cycle `arena → i_rwsem → mmap_lock → arena` reached from the
other end, and the arena write therefore happens after `iterate_dir` returns and
the rwsem is dropped. It must not panic, because a Rust panic unwinding into C
is `BUG()` on this kernel, so every fallible step records an error and returns
false; no `?`, no `unwrap`. And it must not allocate under the rwsem, so the
output vector is preallocated to the full budget before `iterate_dir` is called
and running out of room is pure arithmetic returning false — which is precisely
what false means to `iterate_dir`.

Resumption conflicts with a stated koru rule. `iterate_dir` reads and writes
`file->f_pos` and ignores whatever is in `ctx->pos`, so `READ`'s caller-owned
offset escape is not available. Two answers together: `off` is a resume cookie
applied with `vfs_llseek` before iterating, with the next cookie returned in
`extra`, which keeps position in the SQE and makes an op restartable after
cancellation or teardown; and the handle is serialised with a per-entry busy
flag claimed at submit and released at completion, so a second concurrent
`READDIR` on one handle gets `-EBUSY`. That is koru's own established answer to
userspace naming the same resource twice, and Notes' argument for the slot
bitmap transfers verbatim. It means `OpWork` gains a `held_handle` beside
`held_slot`, released on every completion path including cancellation and
teardown — a second instance of T24's trap and the thing most likely to be got
wrong.

State the new invariant plainly: **`READDIR` is the first opcode that mutates
shared per-file state, and it is serialised per handle for that reason.**

The on-slot entry format is modelled on `linux_dirent64` with its annoyances
fixed, since there is no compatibility to keep: a gap-free, eight-aligned,
twenty-four-byte header carrying inode number, a per-entry resume cookie, record
length, name length and type, followed by the name, a NUL and padding to eight.
Eight-aligned means userspace reads the inode and cookie in place rather than
copying out, which is why every libc copies `linux_dirent64` out. The per-entry
cookie is what makes partial consumption possible: process three of eight and
resume from the third entry's cookie. NUL-terminate despite the length field —
one byte, and it saves every C caller a copy — but the length is authoritative,
and a name containing a NUL means a corrupt filesystem, so skip the entry and
count it in a flag rather than truncating silently. `res` is total bytes written
and `res == 0` means end of directory, mirroring `READ`'s convention exactly. A
handle whose `f_op->iterate_shared` is `None` gives `-ENOTDIR`; `OPEN` already
permits directories, so no `OPEN` change.

Done test: five hundred known entries with a slot small enough to force several
rounds, assembled and compared as a **set** against `readdir(3)`, dot and
dot-dot included. Resumption: stop after one round, resubmit with the returned
cookie, assert nothing duplicated or lost; then resume from a specific entry's
own cookie and assert iteration restarts after it. Two concurrent reads of one
handle giving one `-EBUSY`, where deleting the per-handle flag must fail the
test — and it will fail as duplicated or missing entries rather than as a crash,
so the assertion has to be on content, not errno. Cancelling a queued `READDIR`
frees both the slot and the handle, which is the `held_handle` regression test.
Then the deliberate breakage: take the arena lock inside the callback and
require a circular-locking report under the dmesg gate. Notes lists holding the
arena mutex across `kernel_read` among its four verified breakages; this is its
sibling and deserves the same treatment. Heavy phase.

## Phase 7 — the Braam-compatible surface

Every task here is done twice, once in Rust and once in C++, and the two share
no code. That is the whole point of having two bindings and it is an existing
rule, so the done test for each task runs in both languages.

C++ names live in `namespace koru`. `user/cpp/include/koru/braam.hpp` hoists the
types and free functions to global scope, so a Braam source compiles with one
added include and koru callers who want the namespace keep it. Braam itself has
no namespaces at all, by a documented rule about weak comdat symbols in a
freestanding build; that rule is a wasm artefact and does not apply here, but
the spelling compatibility it implies is worth one header.

The scope boundary, stated here so it is not relitigated. Out: the terminal cell
grid and everything reaching it, the host services (`fetch_url`, `ws_connect`,
`pick`, `clip_get`, `fexport`, `inflate`, `verify_sig`), and processes, pipes,
signals and job control. Also out: Braam's freestanding constraints — no libc,
the 512-byte coroutine frame rule, `heap_new` instead of `new` — which are wasm
artefacts. `cwd_get` and `cwd_set` are implemented with ordinary POSIX calls and
documented as not going through the ring, because koru has no opcode for them
and inventing one buys nothing.

Two semantic adaptations, recorded up front. koru's `DELAY_NS` is capped at one
hour while Braam's `sleep_for` takes a millisecond count reaching fifty days, so
the binding chunks it. And `READ` reports end of file as `res == 0` while
Braam's `read_chunk` reports it as `Err(Closed)`, so the mapping happens in the
wrapper — which is right anyway, since Braam treats `Closed` as a normal end of
input rather than a failure.

### T35 [M] — the shared vocabulary

`Error` as Braam's fifteen names, carrying the raw errno it was built from so
nothing is lost by the mapping. `Result<T, E>` with `is_ok`, `is_err`, `value`,
`error`, `value_or` and an explicit boolean conversion, plus `TRY`, `TRY_VOID`,
`CO_TRY` and `CO_TRY_VOID`. Those macros are statement expressions and need
`-std=gnu++20`, where Notes.md currently says `c++20`; change it there. Native
`Result` and `?` on the Rust side. Type aliases for `Str`, `Span`, `String` and
`Option`, and the hoisting header.

Done test: a table-driven check that every errno koru can produce maps to
exactly one `Error` and back to a raw errno, run in both languages against the
same table; the `TRY` family verified to propagate without copying; and a source
file written in Braam style, with no namespace qualification anywhere, that
compiles with only `braam.hpp` included.

### T36 [M] — the ambient ring and the runtime entry

Braam programs never name an executor, and that is what makes free functions
work. A thread-local default ring, set up by the runtime entry.
`koru_main(Args) -> task<i32>` in C++ and an attribute macro in Rust, plus
`sync_wait` and `block_on`, plus a fire-and-forget spawn matching Braam's
`proc_spawn` but without its eight-task ceiling, plus an at-exit hook — since a
destructor cannot await and the buffered writers need flushing.

Done test: Braam's hello world, compiled and run unmodified except for the
include line, writing through the ring via T24 and T27. Then the T14 and T20
demos re-expressed against the ambient ring, still byte-identical to each other.

### T37 [M] — the free-function operation layer

The Braam signatures, unchanged: `open_at`, `open_read`, `read_chunk`,
`read_some`, `read_file`, `write_all`, `close_fd`, `dup_fd`, `seek_fd`,
`truncate_fd`, `sleep_for`, `stat_of`, `stat_fd`, `list_dir`, `make_dir`,
`make_dir_all`, `remove_path`, `touch_path`, `make_link`, `read_link`,
`rename_path`, `copy_file`, `copy_tree`, `cwd_get`, `cwd_set`, `clock_now` and
`errln`. Each is a slot acquisition, a submit, an await and a copy out.

`seek_fd` is pure userspace bookkeeping, because `READ` and `WRITE` already
carry an explicit file offset and never touch `f_pos`. That is the one place
koru's design makes a POSIX concept unnecessary rather than merely different,
and it is worth saying so in the binding's own documentation.

Done test: a signature-conformance check that every declaration compiles against
Braam's own prototype for the same name, so a drift in argument order or type is
a compile error rather than a surprise. Then a behavioural matrix per function
against its libc equivalent on a fixture tree.

### T38 [M] — buffered `File` and the iterators

`File` with `open`, `of`, the standard-stream accessors, `get`, `unget`, `read`,
`getline`, `put`, `write`, the `scan_*` family, `flush`, `seek`, `close`,
`detach`, and the sticky-error accessors. `Input`, `LineReader` and `TreeWalk`.
The buffering modes, and the documented promise that `~File` neither flushes nor
closes, because a destructor cannot await.

This is where Braam's awaiter-with-a-fast-path shape earns its keep, and it
earns more here than it does on Braam, because a miss costs an `ENTER` syscall
rather than a scheduler step. `await_ready` returns true whenever the buffer
already holds the answer, and only a genuine refill suspends.

Done test: the fast path is **measured**, not assumed. Read a large file one
character at a time and assert the `ENTER` count is proportional to the number
of refills rather than to the number of characters. Instrument the ring and
assert an exact count, because a version that suspends every time still produces
correct output and would pass any behavioural test. Then the sticky-error idiom:
a read error mid-stream leaves `failed()` true and `err()` exact, and
`Error::Cancelled` maps to exit status 130.

### T39 [M] — the program shell

`Args` with `size`, indexing, `name` and `tail`. `Opts`, `Opt` and `OptParse`
with its synchronous, allocation-free `next`, and `help_asked`. `usage_asked`
and `usage_error` with their fixed exit statuses of 0 and 2. `Civil` and the
calendar conversions with their month and day name tables.

Done test: `OptParse` against Braam's own option-parsing vectors, including
clustered flags, an attached value, a detached value, a bare separator and a
missing argument; the calendar conversions round-tripped across a span of dates
including leap years and the epoch; and the two usage helpers verified to write
to the right stream and return the right status.

### T40 [R] — the portability proof

This is the deliverable for the whole Braam effort, and it replaces "two demos
print the same bytes" with something much harder to fake. Take real programs
from Braam's own `src/cmd` and compile them against koru with the include line
as the only edit. The candidates, by ascending difficulty: `echo`, `basename`,
`dirname`, `pwd`, `seq`, `sleep`, `touch`, `mkdir`, `rm`, `ln`, `truncate`,
`date`, `cat`, `wc`, `head`, `tail`, `tr`, `cut`, `uniq`, `cmp`, `tee` and
`grep`.

Done test: a stated number of Braam programs compile with no source change
beyond the include, and each produces byte-identical output to its coreutils
equivalent on a fixture tree, under ASan and UBSan. Any program that needs a
source change is either a scope boundary already declared in this phase's
preamble or a gap in the surface, and this task records which of the two it was
rather than quietly patching the source. Then the Rust half: a handful of the
same programs written as idiomatic Rust against the same function names,
producing the same bytes. That is the language-neutrality claim tested on a real
surface rather than on one demo.

## Phase 8 — only if justified

### T22 [R] — shared mmap'd SQ/CQ

`Atomic<u32>` indices behind a `features` bit, keeping the ioctl path as a
fallback. **Requires first solving research finding 2** in [Notes.md](Notes.md):
obtaining a legitimate stable kernel virtual address into a `Page`, which may
mean patching `rust/kernel/page.rs`. Re-examine whether it is worth it at all —
with one `ENTER` per submit, the only win is syscall-free CQE reading. Phase 6
widened the surface this would have to prove in both modes, so its cost went up
while that stated benefit did not.

Done test: the whole T4–T12 suite plus Phase 6 passes in both modes, fuzz
included, in **both** language bindings.

### T23 [R] — eventfd and tokio bridge

eventfd registration plus a tokio `AsyncFd` bridge. Only if tokio integration
becomes a goal, and try the pure-userspace waiter thread first. Note this is the
point at which the "`await_suspend` must not touch `this`" rule stops being
theoretical — though T26's wake callback already made a sibling of it real in
the kernel.

Done test: a koru op and a tokio TCP read complete in the same tokio runtime,
with the koru side making no progress until the eventfd fires. Then the rule
this task exists to stress: a coroutine resumed from the waiter thread while
`await_suspend` is still on the submitting thread, 100k iterations under TSan,
with a deliberately reintroduced touch of `this` after publishing the handle
shown to fail it.

## Verification

Every task above gates on its own done test. The overall end-to-end sequence,
once everything lands:

1. `make -C test && scripts/run.sh` — the kernel module's own check, in a VM.
   This covers the fuzz, the kmemleak scan past its minimum object age, the
   unprivileged creds cases, the lockdep breakages, and a clean `rmmod`.
2. `cargo test -p koru-sys` — ABI round-trip and per-opcode integration tests
   (T13).
3. `cargo test -p koru` — the futures, the executor, the drop-safety loop and
   the Braam surface (T14, T15, T35–T39).
4. `cargo run --example read_file` — the T14 demo: opens and reads a real file
   through coroutines while timers complete out of order.
5. `cmake -B build -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`, then
   `cmake --build build`.
6. `diff <(./build/abi_dump) <(cargo run -q --bin abi_dump)` — empty (T16).
7. `ctest --test-dir build` — the C++ test matrix (T17), abandonment path
   (T18), symmetric transfer (T19), drop-safety loop (T21) and the Braam
   surface (T35–T39), all under ASan and UBSan.
8. `./build/examples/read_file` — the C++20 demo (T20). Output must match step
   4 byte for byte.
9. `ctest --test-dir build -L portability` — the Braam programs compiled and
   diffed against coreutils (T40).
10. Run steps 4 and 8 **concurrently** (T21). Both must still be correct.

The dev kernel must have KASAN, `PROVE_LOCKING` and `DEBUG_KMEMLEAK` on from day
one; they pay for themselves in the first week.
