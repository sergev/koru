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
symmetric transfer, which T35 below required independently. `Result` with
`TRY`/`CO_TRY` is what Notes.md already proposes. The names are already
idiomatic in both languages. Two things fit better here than on Braam itself:
Rust's `poll` returning ready *is* the awaiter fast path Braam has to simulate,
and that fast path earns more on koru, because a miss costs a syscall rather
than a scheduler step.

Out of scope, permanently: the terminal cell grid, the host services
(`fetch_url`, `ws_connect`, `pick`, `clip_*`, `inflate`, `verify_sig`), and
processes, pipes, signals and job control. Also Braam's freestanding rules — no
libc, no namespaces, the 512-byte frame budget — which are wasm artefacts.
`cwd_get` and `cwd_set` use ordinary POSIX calls and say so in the binding's own
documentation.

Two adaptations. `DELAY_NS` caps at one hour while Braam's `sleep_for` takes
milliseconds up to fifty days, so the binding chunks it. And `READ` reports end
of file as `res == 0` where `read_chunk` reports `Err(Closed)`, so the wrapper
maps it.

## Reading this list

Numbers are execution order, and they were renumbered once when the Braam
decision landed. If you renumber again, fix the references in
[README.md](../README.md), [CLAUDE.md](../CLAUDE.md), [Notes.md](Notes.md) and
`test/`.

Rust lives in a Cargo workspace under `user/`: `koru-sys` holds the ABI structs,
the ioctl wrappers and `abi_dump`; `koru` holds the futures, the executor and
the surface. C++ lives in `user/cpp`, with names in `namespace koru` and
`user/cpp/include/koru/braam.hpp` hoisting them to global scope, so a Braam
source compiles with one added include.

The two bindings share no code. That is the point: the second one exists to show
the ABI is language-neutral, so resist factoring anything across them. The Rust
surface is built first and the C++ surface transcribes it, because the design
will churn and churning it twice is the same mistake this plan avoids for the
ABI.

## Phase 4 — the Rust binding

### T13 [M] — `koru-sys` crate

`#[repr(C)]` ABI structs, ioctl wrappers, `Ring::{setup, enter, mmap}`, and
`size_of` assertions against the kernel's. Plus the errno-to-`Error` table,
which both bindings need and which is ABI-adjacent.

Done test: the T4–T11 tests re-expressed as Rust integration tests, passing.

### T14 [M] — ABI conformance and `abi_dump`

Pulled ahead of the rest of the C++ work, because this is an ABI task rather
than a C++ one, and it is the only thing that keeps `koru_abi.rs` and
`koru_abi.h` in step while the kernel phases add eleven opcodes and two structs
to both.

Create `user/cpp/include/koru_abi.h` from `test/koru_abi.h`, with
`static_assert` on every `sizeof`, `offsetof` and opcode value. The current
mirror carries the structs but none of the `KORU_MAX_*` caps, the three
`*_FLAGS_ALL` masks or `KORU_CQE_F_MORE`; add those. Then an `abi_dump` binary
on each side, emitting a canonical text dump of the whole surface.

Done test: the `diff` of the two dumps is empty, and perturbing one field in
either file makes it fail. Verify that, or the test proves nothing.

### T15 [R] — futures and executor

Op slab keyed by `(index, generation)`, `Future` impls, and a single-threaded
executor whose `park()` is `ENTER`. `OpState` owns the `BufSlot`; the future
owns only the cookie.

Done test: this async block prints the file while timers complete out of order.
**This is the deliverable for the phase.** Output goes through an ordinary
`println!` — writing through the ring needs T17 and T19, and is T21's job.

```rust
async {
    let h = open("/etc/hostname").await?;
    let (n, buf) = read(h, buf).await?;
    println!("{}", str::from_utf8(&buf[..n])?);
    close(h).await?;
}
```

### T16 [R] — drop safety

Done test: race a `read` future against a timer and drop it mid-flight. Assert
that a `CANCEL` is submitted, that the slot is *not* back in the free pool until
the target's CQE lands, and that a later op on that index does not get `-EBUSY`.
100k iterations under ASan.

## Phase 5 — kernel: reaching stdout

The kernel reopens for a reason. Braam's hello world is
`co_await write_all(SYS_STDOUT, ...)`, and koru has no `WRITE`, while `OPEN`
refuses anything but regular files and directories on purpose, because a
blocking read in a kworker cannot be interrupted. Stdout cannot reach the ring
today.

One finding governs every kernel task in this plan. **`override_creds` is not
exported.** `prepare_creds` and `abort_creds` are, but neither can install a
cred set, so no out-of-tree module can defer a creds-sensitive operation on this
kernel. Every path-walking op below is inline, permanently; Notes' finding 3 is
not a "for now".

Three obligations apply to every kernel task rather than being repeated in each:

- `KoruParams::features` gains a bit per capability, so userspace probes instead
  of submitting and watching for `-EINVAL`. This is what `features` was for.
- The fuzzer grows with each opcode: a per-opcode allowed-`res` set, a
  per-opcode `extra` oracle replacing the blanket `extra == 0` assertion, and —
  for anything touching the filesystem — a per-run temp directory plus a path
  generator structurally unable to emit `..` or an absolute path. That
  sandboxing is a property of the test, not the kernel, and it is the most
  important safety property in the plan.
- Any new `#[repr(C)]` is mirrored in `koru_abi.h` and covered by T14's diff.

No kernel task bumps `KORU_ABI_VERSION`. New opcodes are additive under the
existing "unimplemented completes `-EINVAL`" rule. A new bit in an existing
`*_FLAGS_ALL` mask only relaxes a rejection, so it is safe in both directions.
Per-opcode meanings for `Cqe::extra` are within its documented contract. New
data-plane structs change no existing size or offset.

### T17 [M] — `KORU_OP_WRITE`

`len` bytes from slot offset 0, written at file offset `off` of `handle`.
Deferred to the workqueue, mirroring `READ`. New `check_writable` guarding
`FMODE_WRITE | FMODE_CAN_WRITE`, regular files only for now, and the same
`f_op->write` / `write_iter` shape check that keeps `not supported for file` out
of the log. Use `kernel_write`, not `__kernel_write`, which skips
`rw_verify_area` and freeze protection. Chunk page by page; reusing `read_slot`
would allocate a whole megabyte. Hold the arena mutex only around each
`read_raw`, never across `kernel_write`.

`O_APPEND` is the non-obvious one. `kernel_write` inherits `IOCB_APPEND` from
`f_iocb_flags`. koru's `open_flags` has no `O_APPEND`, but an adopted handle
(T19) can, and then `off` is silently ignored. Document it.

`off` now means: a file offset on `READ` and `WRITE`, a within-slot offset on
`OPEN` and `CHECKSUM`, nanoseconds on `DELAY_NS`, a target cookie on `CANCEL`.
Phase 7 adds four more meanings. The per-opcode doc comments in `koru_abi.rs`
are a second implementer's only defence, so make that list explicit there.

Done test: a known pattern written to a temp file and read back with `pread(2)`;
placement at a non-zero `off` into a sparse file; the rejection matrix
(read-only handle to `-EBADF`, directory handle, zero or oversized `len`,
out-of-range `slot`, non-zero `rsvd0`, unknown flag bit), each shown to fail
when its check is deleted; two concurrent writes to one slot giving one
`-EBUSY`. Then the one that matters: cancel a queued `WRITE` and assert the slot
is freed. That fails if `KORU_OP_WRITE` is missing from `OpWork::held_slot`,
which is this task's most likely bug and which otherwise shows up only as every
later op on that index getting `-EBUSY` with nothing saying why. Heavy phase.

### T18 [M] — `KORU_O_NONBLOCK` and the non-regular-file gate

`RWF_NOWAIT` is not the mechanism. `kiocb_set_rw_flags` returns `-EOPNOTSUPP`
unless `f_mode & FMODE_NOWAIT`, and a `filp_open`'d FIFO never has that bit — it
is set only by `pipe(2)`, `sock_alloc_file`, eventfd, timerfd, signalfd and
userfaultfd. A tty never sets it, and `n_tty` checks `f_flags & O_NONBLOCK` and
nothing else. The mechanism is `O_NONBLOCK` at open time.

Add the flag to `KORU_OPEN_FLAGS_ALL`, translate it in `open_flags`, and relax
`check_readable` and `check_writable` to admit a non-regular file when and only
when `f_flags & O_NONBLOCK` is set. **koru never sets or clears that bit on a
file it did not open**: for an adopted descriptor the `struct file` is shared
with the rest of the process, so flipping it on stdin changes behaviour for
everything else holding that open file description.

This also closes a denial of service Notes.md lists as accepted. A blocking
`filp_open` on a peerless FIFO stalls the whole ring *before* the file-type
check can run, and the file-type restriction does not close it. An opt-in flag
does, with nothing changing for existing callers.

Done test: opening a peerless FIFO with `KORU_O_NONBLOCK` returns promptly,
which is the assertion that closes the stall; `READ` on it gives `-EAGAIN`; a
FIFO handle opened without the flag is rejected at submit with `-EINVAL`, and
deleting that check must make the test **hang** rather than fail, so arm the
alarm and keep stdout line-buffered. The regular-file restriction was one of two
independent readability guards, so re-verify the surviving `f_op` guard on its
own rather than assuming it.

### T19 [M] — `KORU_OP_ADOPT_FD`

A handle for an already-open descriptor, so stdin, stdout and stderr are usable.
An opcode rather than a fourth ioctl, so it inherits C1, E1, the `user_data`
echo, batching and the fuzzer's oracle for free. `off` carries the descriptor as
a `u64` that must be at most `i32::MAX`, so `AT_FDCWD`-style magic numbers never
reach `fget`; `len`, `slot` and `handle` must be zero. Using `off` rather than
`handle` keeps "`handle` is a koru handle except on `OPEN`" from gaining a third
exception.

Inline, but for a different reason from `OPEN`: `fget` resolves against
`current->files`, which in a kworker is the kthread's table. Use the owning
`LocalFile::fget` plus `assume_no_fdget_pos`, never `fdget`, whose light
reference is valid only while the fd table cannot change — this reference
outlives the ioctl by design and a kworker on another CPU will use it. The
safety condition holds because the ioctl path takes `fdget`, not `fdget_pos`;
put that in the comment verbatim. `O_PATH` needs no check, since `fget` is
`__fget(fd, FMODE_PATH)` and already returns NULL for them.

The security statement is the opposite of `OPEN`'s and stronger. `fget` performs
no permission check, so **`ADOPT_FD` grants koru no authority the submitting
task does not already hold.** Worth stating precisely, because the reflex is to
assume otherwise.

Two rejections whose absence is catastrophic rather than subtle. Any koru
descriptor, not merely our own, gives `-ELOOP`: the ring's file holds
`Arc<RingCtx>` as private data and the handle table lives inside `RingCtx`, so
adopting it makes `release` unreachable, the module permanently unloadable and
the arena leaked. Two rings do the same in two hops, so comparing against our
own file alone is insufficient; compare `f_op`, which means threading the ring's
`&File` into `dispatch`. Do that here. And a non-regular adopted descriptor
without `O_NONBLOCK` must be refused for `READ` and `WRITE` by T18's gate, which
is why this task follows it: shipped first, the obvious `cat` demo wedges a
kworker on its first read.

Done test: adopt descriptor 1 where stdout is a pipe under the harness, write
through the handle, parent reads the bytes. That is "stdout is usable", tested.
Own ring fd and a second ring's fd both give `-ELOOP`, and with the check
deleted `rmmod` must fail and one ring must leak — measured by the
release-timing gate and field 1 of `/proc/sys/fs/file-nr`, not by kmemleak,
since our own table still references the file. Bad, closed and out-of-range
descriptors give `-EBADF`. The privilege mirror to `OPEN`'s test, asserting the
*opposite* outcome: a child dropped to nobody adopts a descriptor the parent
opened on a root-only file and **succeeds**. Keep both in one section, because
if this case ever starts failing, something has begun re-checking permissions at
use time. Lifetime: adopt, `close(2)` the descriptor, read through the handle
and still get data, which catches an `fdget` mistake at once. Then 2,000
adopt-and-close cycles against `file-nr`, heavy phase.

## Phase 6 — first milestone

Two tasks, and at the end of them Braam's hello world runs through koru. This is
the first point where the project's claim is demonstrated end to end rather than
asserted, and it arrives well before the plan is finished.

### T20 [M] — the vocabulary, Rust

`Error` as Braam's fifteen names, each carrying the raw errno it was built from
so the mapping loses nothing. Native `Result` and `?`. Aliases where Braam's
names differ from Rust's.

Done test: a table-driven check that every errno koru can produce maps to
exactly one `Error` and back to a raw errno, sharing the table with T13's.

### T21 [M] — the ambient ring and runtime entry, Rust

Braam programs never name an executor, and that is what makes free functions
work. A thread-local default ring set up by the runtime entry, an attribute
macro for `main`, `block_on`, a fire-and-forget spawn matching `proc_spawn`
without its eight-task ceiling, and an at-exit hook — a destructor cannot await,
and the buffered writers need flushing.

Also the write half of the operation layer: `write_all` and `close_fd`, enough
for the milestone. The rest is T30.

Done test: Braam's hello world, compiled and run with the include line as the
only change, writing through the ring via T17 and T19. Then T15's demo
re-expressed against the ambient ring, same output.

## Phase 7 — kernel: the rest of the surface

### T22 [R] — `KORU_OP_POLL_ADD`

Taken next because it is the biggest unknown in the plan, and because the
non-blocking work of T18 is still fresh. Its failure mode is a use-after-free
from softirq, so finding it infeasible here costs far less than finding that out
after the surface is built on it.

Single-shot; multishot stays foreclosed by admission control. `handle` names the
file, `len` carries a requested-event mask in koru's own bit values, `off` and
`slot` must be zero, `res` is the returned mask — always non-negative, so no
collision with errnos.

`vfs_poll`, `init_poll_funcptr` and `init_waitqueue_func_entry` are static
inlines with no Rust helper; each is two or three field stores. The poll mask
constants are hardcoded with a comment, as `FMODE_READ` already is, because
bindgen cannot evaluate their casts. A file whose `f_op->poll` is `None`
completes immediately as always-ready, which is correct for a regular file.
Allow exactly one `poll_wait` per arm; a second sets a flag and the op completes
`-EOPNOTSUPP`, rather than reimplementing io_uring's double-entry machinery.

The wake callback is the risk, and it is a new kind for this module. It runs
with the waitqueue head's spinlock held and, for a socket, in **softirq**.
`kernel::sync::SpinLock` is plain `spin_lock` with no irq-safe variant in this
tree, so the callback may not call `complete()`, may not take `pending`, may not
`fput` and may not `remove_wait_queue`. It wins a one-shot atomic token, stashes
the mask, and enqueues, which is IRQ-safe; the kworker does the removal and the
completion in process context. This is a sibling of "`ENTER` must never touch a
`UserSlice` while holding the ring `SpinLock`" and belongs beside it in Notes.

Two consequences elsewhere. `CANCEL` needs a second arm: an armed poll is on no
workqueue, so `cancel_delayed_work` returns false and the existing code would
answer `-EALREADY` for an op that is merely waiting, for ever. The same token
decides it — the winner disarms, removes and adopts exactly one reference; the
loser answers `-EALREADY`. That is the only structure preserving the refcount
rule Notes verified by breaking in both directions. And `cancel_all` must disarm
every armed poll **inside** the `pending` lock, before the registry is swapped
out and the files are dropped. Backwards is not a leak, it is a use-after-free
walking a freed list node from softirq. The waitqueue head stays alive only
because the op holds an `ARef<File>`, which is the whole safety argument and is
what makes the drop order load-bearing.

Explicit `POLL_ADD` rather than io_uring-style auto-arm on `-EAGAIN`. Auto-arm
does not remove any of this machinery, it adds a state machine on top; it
multiplies the refcount rule across five states on the module's most delicate
code; it reopens the CQ-reservation exhaustion the fuzzer already found, since a
read on an empty FIFO would hold its reservation and slot indefinitely; and it
hides the wait from the coroutine. Revisit only after multishot.

Done test: `POLL_ADD` on a regular-file handle completes immediately with the
default mask, proving the no-`poll` path. Then the deliverable: arm on a FIFO,
assert no completion, have a helper thread write a byte, assert completion with
the read bit, then read that byte. Cancel an armed poll: target `-ECANCELED`,
cancel `0`, and then writing to the FIFO must produce **no** CQE at all —
omitting `remove_wait_queue` gives a KASAN use-after-free under the dmesg gate,
and omitting the token gives a double completion that breaks C1 and that the
fuzzer's consumed-equals-reaped assertion sees directly. Close the ring fd with
a poll armed, write to the FIFO, `rmmod` inside the release-timing gate. Run a
cancel-versus-wake race loop gated on reaching both arms. Then the deliberate
breakage: call `complete()` from the wake callback and require `PROVE_LOCKING`
to report inconsistent lock state and the dmesg gate to fail the run. Lockdep is
the real oracle here.

### T23 [M] — `KORU_OP_STAT` by handle, and `KoruStat`

`res` plus `extra` is sixteen bytes and a `kstat` is about a hundred and fifty,
so the result goes in the slot. `slot` is the destination, `off` a within-slot
offset, `len` the caller's buffer size — which doubles as version negotiation,
since the kernel writes `min(len, sizeof(KoruStat))` and returns that in `res`.
`extra` carries `kstat.result_mask`, the fields actually valid.

`KoruStat` is a new `#[repr(C)]` in `koru_abi.rs`, held to the same offset
assertions and `AsBytes`/`FromBytes` discipline as `Sqe`: fixed 64-bit fields,
times as second-plus-nanosecond pairs, device numbers as explicit major and
minor rather than the internal `dev_t`, and a `reserved` array the kernel zeroes
so a later field is a carve-out exactly as `handle_count` was. Translate `kstat`
field by field, never by `transmute` — it is kernel-internal, and the shortcut
looks fine until a kernel bump.

Deferred, because `vfs_getattr` blocks on NFS and FUSE. It holds a slot, so it
joins `held_slot`: T17's trap, second instance.

Two easy mistakes. The kernel must zero the whole destination before filling it,
because `READ`'s "the rest of the slot is the caller's own data" argument does
not transfer — a partially filled struct makes the caller read stale bytes as if
they were kernel-reported values. And `kstat.uid` is a `kuid_t`, meaningful only
through `from_kuid(current_user_ns(), ...)`; in a kworker that namespace is
init's, so a deferred stat silently reports wrong numbers inside a container.
Carry the submitter's user namespace in `OpWork` and translate in the worker;
capturing it at submit time is impossible, since the stat has not happened yet.
This is finding 3's quiet sibling — wrong numbers rather than a privilege
escalation, so nothing catches it by accident.

Done test: every field cross-checked against `fstat(2)` on the same path, which
is the whole test and is cheap. A `len` smaller than the struct returns
`res == len` and leaves a sentinel at `off + len` untouched. The rejection
matrix, and two concurrent stats on one slot giving one `-EBUSY`. `extra` equals
the returned mask. Zeroing: pre-fill the slot with a poison byte and assert
every byte of the struct is a real field or a zero. The uid mapping is asserted
against the caller's own view, with a comment saying plainly that this passes on
the dev VM and would fail in a container if the namespace were not carried — a
test that only fails where nobody runs it is not evidence.

First opcode to set `extra`, so this is where the fuzzer's blanket `extra == 0`
oracle becomes per-opcode. Forgetting makes the fuzz green for the wrong reason.

### T24 [R] — `kern_path` plumbing: `TRUNCATE`, `UTIMES`, `READLINK`

Three simple path operations, chosen to land the shared infrastructure before
anything runs under a dentry lock. All inline, permanently.

`linux/namei.h` is absent from `rust/bindings/bindings_helper.h`, so `kern_path`
and every `LOOKUP_*` are missing from `bindings::` even though they are
exported. New `kernel/koru_path.rs` holds every hand-declared
`unsafe extern "C"` prototype in one place, each with its C signature quoted
verbatim above it. Nothing checks these — no `static_assert` can help — so the
file carries a written obligation to re-check on every kernel bump, and Notes
records it.

Guard types with `Drop` for `mnt_want_write`/`mnt_drop_write` and for
`path_put`. An early return through `?` that skips `mnt_drop_write` pins the
filesystem against read-only remount until reboot. One of the few places Rust
genuinely helps; take it.

`TRUNCATE` puts the new length in `off`, since it is a file offset and there is
no ambiguity, and requires `len` to be zero; `vfs_truncate` does its own
`mnt_want_write` and permission check. `UTIMES` reads a two-timespec struct from
the slot immediately after the path, reusing the kernel's own `UTIME_NOW` and
`UTIME_OMIT` sentinels, which `vfs_utimes` checks directly.

`READLINK` is **not** `vfs_readlink`, whose buffer is a `char __user *` and
which would both violate the central invariant read literally and fault. It is
`kern_path` without `LOOKUP_FOLLOW` — the entire point — then `vfs_get_link`,
then `do_delayed_call`, whose omission leaks a page per page-backed symlink.
NUL-terminate in the slot and return the length excluding the NUL.

Done test: each cross-checked against its libc equivalent — `st_size` after
truncate, `st_atim` and `st_mtim` after utimes, `readlink(2)` against the slot.
Then the creds test, never written for anything but `OPEN`: a child dropped to
nobody truncates a root-owned file and gets `-EACCES`, after which the op is
deliberately deferred to the workqueue and must be seen to succeed as root. That
is the regression test for the whole inline-because-of-creds rule and it is the
most valuable assertion in the plan. Then 2,000 readlinks of a long symlink in
the heavy phase with `do_delayed_call` dropped, which kmemleak must report.

### T25 [M] — `KORU_OP_STATX_AT`

Stat by path: `kern_path` with `LOOKUP_FOLLOW`, the same `vfs_getattr` and
`KoruStat` as T23, then `path_put`. Inline, for `OPEN`'s reasons — `current->fs`
for relative paths, `inode_permission` per component, the LSM seeing the right
task. Nearly free once T23 and T24 have landed.

Path handling reuses `do_open`'s recipe verbatim: clamp to `PATH_MAX`, copy
under a slot claim, reject an embedded NUL in the kernel copy, append our own.
Path in and struct out use one slot under a single claim — the path is consumed
into a `KVec` before anything is written, so there is no acquisition-order
question. Document that the op overwrites the path it was given.

Done test: a known path matches `stat(2)` field for field; a path the
dropped-privilege child cannot traverse gives `-EACCES`, reusing T24's creds
harness and shown to fail when the op is deferred.

### T26 [M] — `KORU_OP_MKDIR` and `KORU_OP_SYMLINK`

The `start_creating_path` and `end_creating_path` happy path. `vfs_mkdir`
returns a possibly different dentry, and it is that one, not the one
`start_creating_path` handed over, that must go to `end_creating_path`;
confusing them unlocks the wrong inode. `mnt_idmap` is a static inline doing one
`READ_ONCE` and is reimplemented in Rust.

`MKDIR` carries its mode in `handle`, free on this opcode exactly as `OPEN`'s
flags are, masked to the permission bits with everything else rejected; the VFS
applies the umask. Consequence worth noting: Notes.md records that there is no
`O_CREAT` because no field can carry a creation mode, and this retires that
reason.

`SYMLINK` needs two paths in one slot: two NUL-terminated strings back to back
with `len` covering both. Read exactly `len` bytes, require **exactly one**
interior NUL, split there, append our own NUL to the second, reject either side
empty. A different parse, not a relaxation of the embedded-NUL rule. Factor it
into one helper, since T28 needs it. Do not use `off` as a second length — it
collides with `off`'s within-slot meaning.

Pass NULL for `delegated_inode` throughout T26 to T28. `try_break_deleg` with
NULL returns `-EWOULDBLOCK` without taking a reference, so there is no `iput`
bookkeeping; the cost is that NFS delegations surface as `-EWOULDBLOCK` rather
than being broken. Right trade for a first cut, and documented.

Done test: both verified with `stat(2)` and `readlink(2)`; the creds case
against T24's harness; the two-path parse rejection matrix — zero interior NULs,
two, one at position zero, one at the last byte — each shown to fail when its
check is deleted. Then 2,000 mkdir-and-rmdir pairs in the heavy phase followed
by an assertion that the filesystem can still be remounted read-only, which is
the only instrument that sees a leaked write count.

### T27 [R] — `KORU_OP_UNLINK` and `KORU_OP_RMDIR`

`start_removing_path` is not exported, and the only exported variant takes a
`__user` name and is unusable. So the sequence is assembled by hand: split the
path at its last separator in the module, `kern_path` the parent with
`LOOKUP_FOLLOW | LOOKUP_DIRECTORY`, `mnt_want_write`, build a `qstr` whose hash
`start_removing` computes for you, `start_removing`, `vfs_unlink` or
`vfs_rmdir`, `end_dirop` — exported and bound, the one lucky break — then
unwind. That hand-assembly is why these are separate from T26.

Rejecting what `filename_parentat` would have rejected as a non-normal last
component — empty, `.`, `..`, a trailing separator — is our own job here.
Missing it is how removing a path ending in `..` does something surprising.

Done test: both verified with `access(2)`; `rmdir` on a non-empty directory
giving `-ENOTEMPTY`, `unlink` on a directory giving `-EISDIR`, `rmdir` on a
regular file giving `-ENOTDIR`; a path ending in `..` giving `-EINVAL` from our
own check, which when deleted must fail *differently* rather than not at all;
the creds case; and T26's write-count balance assertion.

### T28 [R] — `KORU_OP_RENAME`

`start_renaming`, which calls `lookup_one_common` on both sides so there is
again no hash work, then `vfs_rename` and `end_renaming`, with `mnt_want_write`
on both mounts and a cross-mount rejection of `-EXDEV` before anything starts.
By far the most intricate of the path operations, which is why it is alone and
last. Reuses T26's two-path parse. `struct renamedata` is already bound.

Done test: rename within a directory, across directories, onto an existing file,
and across mounts for `-EXDEV`; the creds case; the write-count balance
assertion on both mounts.

### T29 [R] — `KORU_OP_READDIR` and `KoruDirent`

A `#[repr(C)]` wrapper whose first field is a `dir_context`, recovered in the
`filldir` callback with the `container_of!` macro `OpWork::delayed_work` already
uses. Put the context first so the offset is zero, but use the macro anyway, so
a later field reorder cannot silently break it.

Three constraints on that callback, none obvious, all forced. It runs with the
inode's `i_rwsem` held for read, so it **must not take the arena mutex at all**.
That is Notes' lockdep cycle reached from the other end, and the arena write
therefore happens after `iterate_dir` returns. It must not panic, because a Rust
panic unwinding into C is `BUG()` on this kernel, so every fallible step records
an error and returns false; no `?`, no `unwrap`. And it must not allocate under
the rwsem, so the output vector is preallocated to the full budget and running
out of room is pure arithmetic returning false — which is precisely what false
means to `iterate_dir`.

Resumption conflicts with a stated koru rule. `iterate_dir` reads and writes
`file->f_pos` and ignores `ctx->pos`, so `READ`'s caller-owned-offset escape is
not available. Two answers together: `off` is a resume cookie applied with
`vfs_llseek` before iterating, with the next cookie returned in `extra`; and the
handle is serialised with a per-entry busy flag claimed at submit and released
at completion, so a second concurrent `READDIR` on one handle gets `-EBUSY`.
That is koru's own answer to userspace naming the same resource twice, and
Notes' argument for the slot bitmap transfers verbatim. `OpWork` gains a
`held_handle` beside `held_slot`, released on every completion path including
cancellation and teardown — T17's trap, third instance, and the most likely bug
here.

State the new invariant plainly: **`READDIR` is the first opcode that mutates
shared per-file state, and it is serialised per handle for that reason.**

Entry format, modelled on `linux_dirent64` with its annoyances fixed since there
is no compatibility to keep: a gap-free, eight-aligned, twenty-four-byte header
carrying inode number, a per-entry resume cookie, record length, name length and
type, then the name, a NUL and padding to eight. Eight-aligned means userspace
reads the inode and cookie in place rather than copying out, which is why every
libc copies `linux_dirent64` out. The per-entry cookie makes partial consumption
possible. NUL-terminate despite the length field, but the length is
authoritative; a name containing a NUL means a corrupt filesystem, so skip the
entry and count it in a flag rather than truncating silently. `res` is total
bytes written and `res == 0` means end of directory, mirroring `READ`. A handle
whose `f_op->iterate_shared` is `None` gives `-ENOTDIR`; `OPEN` already permits
directories, so no `OPEN` change.

Done test: five hundred known entries with a slot small enough to force several
rounds, assembled and compared as a **set** against `readdir(3)`, dot and
dot-dot included. Resumption: stop after one round, resubmit with the returned
cookie, assert nothing duplicated or lost; then resume from a specific entry's
own cookie and assert iteration restarts after it. Two concurrent reads of one
handle giving one `-EBUSY`, where deleting the per-handle flag must fail the
test — and it fails as duplicated or missing entries rather than as a crash, so
the assertion has to be on content, not errno. Cancel a queued `READDIR` and
assert both the slot and the handle are freed. Then the deliberate breakage:
take the arena lock inside the callback and require a circular-locking report
under the dmesg gate. Heavy phase.

## Phase 8 — the Rust surface

### T30 [M] — the operation layer, Rust

The Braam signatures, unchanged: `open_at`, `open_read`, `read_chunk`,
`read_some`, `read_file`, `dup_fd`, `seek_fd`, `truncate_fd`, `sleep_for`,
`stat_of`, `stat_fd`, `list_dir`, `make_dir`, `make_dir_all`, `remove_path`,
`touch_path`, `make_link`, `read_link`, `rename_path`, `copy_file`, `copy_tree`,
`cwd_get`, `cwd_set`, `clock_now`, `errln`. `write_all` and `close_fd` already
exist from T21. Each is a slot acquisition, a submit, an await and a copy out.

`seek_fd` is pure userspace bookkeeping, because `READ` and `WRITE` carry an
explicit file offset and never touch `f_pos`. That is the one place koru's
design makes a POSIX concept unnecessary rather than merely different; say so in
the binding's documentation.

Done test: a signature-conformance check that every declaration matches Braam's
prototype for the same name, so a drift in argument order or type is a compile
error rather than a surprise. Then a behavioural matrix per function against its
libc equivalent on a fixture tree.

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

## Phase 9 — the C++ binding

The ABI is settled, and so is the surface design. This phase transcribes it,
which is what makes the language-neutrality claim a test rather than an
assertion.

### T33 [M] — `libkoru` synchronous core

RAII `Ring` covering open, `SETUP`, `mmap` and close. `BufPool`, move-only
`BufSlot` with deleted copy operations, raw `submit()` and `reap()`, and
`result<T>` carrying both the Braam `Error` and the raw errno. No coroutines.

Done test: the T4–T11 matrix re-expressed in C++, the same assertions as T13 in
a different language. Any divergence is an ABI ambiguity worth fixing before
coroutines hide it.

### T34 [R] — op slab and awaiter

An `(index, generation)`-keyed slab of `op_state`, plus `await_ready`,
`await_suspend` and `await_resume`. `~op_awaiter` marks the entry `Abandoned`,
moves the `BufSlot` into it, and submits `CANCEL`.

Done test: destroy a coroutine frame with an op in flight. Under ASan there is
no resume of a freed handle, the slot is not recycled until the CQE lands, and a
later op on that index does not get `-EBUSY`.

### T35 [R] — `task<T>`

Lazy `initial_suspend`, **symmetric transfer** on `final_suspend`, move-only,
`unhandled_exception` going to `std::terminate`. Plus `sync_wait(task<T>)`, and
`get_return_object_on_allocation_failure` so a frame-allocation failure yields a
null `task` rather than undefined behaviour — one static member function, and
what makes Braam's pervasive `if (task<T> t = ...)` idiom compile.

Done test: 100,000 nested `co_await`s complete with stack usage *measured* flat,
not assumed. This is the symmetric-transfer regression test and it silently
passes if you write it wrong and only try ten levels. A task destroyed without
being awaited must leak nothing under LSan.

### T36 [R] — C++ executor

Ready queue, a `run()` whose park is `ENTER(min_complete=1, timeout)`, and a CQE
path of slab lookup then resume or discard. Close the same empty-ring deadlock
foot-gun as the Rust side.

Done test: T15's demo in C++, concurrent with delays completing out of order.
Output byte-identical to the Rust demo.

### T37 [R] — C++ drop safety

Done test: a mirror of T16 — race a read against a timer, destroy the frame
mid-flight, 100k iterations under ASan and UBSan.

## Phase 10 — the C++ surface

### T38 [M] — the vocabulary and runtime entry, C++

`Error`, `result<T>`, `TRY`, `TRY_VOID`, `CO_TRY`, `CO_TRY_VOID`. Those macros
are statement expressions and need `-std=gnu++20`, where Notes.md says `c++20`;
change it there. Aliases for `Str`, `Span`, `String`, `Option`, and
`koru/braam.hpp` hoisting everything to global scope. Then the ambient ring,
`koru_main(Args) -> task<i32>`, the spawn and the at-exit hook.

Done test: the errno table from T20, in C++, giving identical results. Then a
source file written in Braam style, with no namespace qualification anywhere,
compiling with only `braam.hpp` included — and Braam's hello world running with
the same output as T21's.

### T39 [M] — the operation layer, C++

T30's function list, same signatures.

Done test: T30's signature-conformance and behavioural matrix, in C++.

### T40 [M] — buffered `File` and the iterators, C++

T31's surface. `~File` neither flushes nor closes, which is Braam's documented
behaviour and not an oversight.

Done test: T31's, including the measured `ENTER` count.

### T41 [M] — the program shell, C++

T32's surface.

Done test: T32's, against the same vectors.

## Phase 11 — the proof

### T42 [R] — the portability proof

The deliverable for the whole Braam effort. Take real programs from Braam's
`src/cmd` and compile them against koru with the include line as the only edit.
By ascending difficulty: `echo`, `basename`, `dirname`, `pwd`, `seq`, `sleep`,
`touch`, `mkdir`, `rm`, `ln`, `truncate`, `date`, `cat`, `wc`, `head`, `tail`,
`tr`, `cut`, `uniq`, `cmp`, `tee`, `grep`.

Done test: a stated number of them compile with no source change beyond the
include, and each produces byte-identical output to its coreutils equivalent on
a fixture tree, under ASan and UBSan. Any program needing a source change is
either a scope boundary already declared above or a gap in the surface, and this
task records which rather than quietly patching the source. Then the Rust half:
a handful of the same programs written as idiomatic Rust against the same
function names, producing the same bytes. That is language-neutrality tested on
a real surface rather than on one demo.

## Phase 12 — only if justified

### T43 [R] — shared mmap'd SQ/CQ

`Atomic<u32>` indices behind a `features` bit, keeping the ioctl path as a
fallback. **Requires first solving research finding 2** in [Notes.md](Notes.md):
a legitimate stable kernel virtual address into a `Page`, which may mean
patching `rust/kernel/page.rs`. Re-examine whether it is worth it at all — with
one `ENTER` per submit, the only win is syscall-free CQE reading, and Phases 5
and 7 widened the surface this must prove in both modes without widening that
benefit.

Done test: the whole T4–T12 suite plus every kernel task above passes in both
modes, fuzz included, in **both** bindings.

### T44 [R] — eventfd and tokio bridge

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
2. `cargo test -p koru-sys` — ABI round-trip and per-opcode tests (T13).
3. `cargo test -p koru` — futures, executor, drop safety and the Rust surface
   (T15, T16, T20, T21, T30–T32).
4. `cargo run --example read_file` — the Rust demo (T21).
5. `cmake -B build -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`, then
   `cmake --build build`.
6. `diff <(./build/abi_dump) <(cargo run -q --bin abi_dump)` — empty (T14).
7. `ctest --test-dir build` — the C++ matrix (T33), abandonment (T34),
   symmetric transfer (T35), drop safety (T37) and the C++ surface (T38–T41),
   under ASan and UBSan.
8. `./build/examples/read_file` — the C++ demo (T36). Must match step 4 byte
   for byte.
9. `ctest --test-dir build -L portability` — Braam's programs against coreutils
   (T42).
10. Steps 4 and 8 **concurrently** (T37). Both must still be correct.

The dev kernel must have KASAN, `PROVE_LOCKING` and `DEBUG_KMEMLEAK` on from day
one; they pay for themselves in the first week.
