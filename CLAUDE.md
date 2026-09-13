# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

## State of the repository

**T0–T30 are done.** The kernel surface was complete at T29 and grew once more
at T30, for `OPEN`'s creation flags. Braam's hello world runs through koru, and
the ring can wait for a descriptor. The module registers `/dev/koru`, configures
a ring with `SETUP`, and submits `NOP`, `DELAY_NS`, `CHECKSUM`, `OPEN`, `READ`,
`WRITE`, `CLOSE`, `CANCEL`, `ADOPT_FD`, `POLL_ADD`, `STAT`, `TRUNCATE`,
`UTIMES`, `READLINK`, `STATX_AT`, `MKDIR`, `SYMLINK`, `UNLINK`, `RMDIR`,
`RENAME` and `READDIR` through `ENTER`, which blocks for completions. The arena
is mmap'd, with slot exclusivity enforced by the kernel. Open files live in a
generational handle table. A queued op can be genuinely dequeued, and
`close(fd)` cancels whatever is still queued. The whole validation surface has
been fuzzed under KASAN, lockdep and kmemleak.

T13 added `rust/sys`: the ABI mirror, the ioctl wrappers, `Ring`, `Arena`,
`BufPool` and the errno table, with the T4–T11 matrix re-expressed as Rust
integration tests. It also moved the whole project onto rustup's rustc 1.98.1.
Everything from here is userspace.

T15 added `rust/runtime`: the op slab, a future per opcode and a
single-threaded executor whose park is `ENTER`. An async block now reads a file
through the ring while timers complete out of order. The crate is
`#![forbid(unsafe_code)]` and depends on nothing from a registry.

T16 added `race`, the crate's first combinator, and falsified the drop path
under it: a `READ` dropped mid-flight keeps its slot until the target's CQE
lands, and the op that reuses that index must not get `-EBUSY`. "Under ASan"
was settled as no nightly toolchain — `forbid(unsafe_code)` plus the kernel's
KASAN plus the `Stats` accounting. doc/Notes.md has the reasoning and the five
perturbations, one of which this test does **not** catch.

T17 added `KORU_OP_WRITE`, the first new opcode since T11 and the first kernel
work since T12. It mirrors `READ`: inline validate, deferred to the workqueue,
one page-sized bounce buffer, the arena mutex never held across the VFS call.
`features` stays zero, because `WRITE` is unconditional. Two of its guards are
not falsifiable yet and one never will be by errno alone; doc/Notes.md says
which and why.

T18 added `KORU_O_NONBLOCK`, so a FIFO can be opened and read. The file-type
gate moved off `OPEN`, which now refuses nothing, onto `check_readable` and
`check_writable`, which admit a non-regular file only when it was opened
non-blocking. Two of its six perturbations fail as a hang rather than an
assertion, and two of them retire gaps T17 had to record as untested.

T19 added `KORU_OP_ADOPT_FD`, so a descriptor the caller already holds becomes
a koru handle and stdout reaches the ring. It grants no authority the caller
lacks, and the check proves that by having an unprivileged child adopt a
root-only descriptor its parent opened. Adopting any koru fd is `ELOOP`, which
is why the ring's own `&File` is threaded into `dispatch`; without it `rmmod`
never succeeds again.

T20 opened the Braam surface with the vocabulary: `koru::vocab` re-exports
`koru-sys`'s `Error`, `Kind` and `Errno`, adds `Result`, `Str`, `Span` and
`SpanMut`, and the `?` conversions from `Errno`, `io::Error` and `EnterError`.
Those impls are in `koru-sys`, because the orphan rule allows them nowhere
else. `Error::closed()` is the only name userspace synthesises, and Rust's `?`
is all four of Braam's `TRY` macros.

T21 added the ambient ring: a thread-local `Runtime`, `#[koru::main]` from the
new dependency-free `koru-macros`, `block_on`, `spawn`, `at_exit`, `Args`, and
the write half of the operation layer, `write_all` and `close_fd`. The standard
streams are re-opened through `/proc/self/fd` before they are adopted, because
T18's gate refuses a blocking non-regular file and koru never sets `O_NONBLOCK`
on a descriptor it did not open. The file position is userspace's own
bookkeeping, which Braam's three-write hello world is what forces. `EPIPE`
joined the errno table as `Closed`.

T22 added `KORU_OP_POLL_ADD`, a third op shape beside inline and deferred:
**armed**. The wake callback runs with the waitqueue head's spinlock held and,
for a socket, in softirq, so it may take no koru lock, complete nothing and
touch no list — it wins a one-shot token and enqueues, and a kworker does the
rest. That token is what `CANCEL` and `release` use too, and `release` must
disarm inside the `pending` lock, before the files are dropped. A file on no
waitqueue is never armed: it completes at once, with `res` 0 where none of the
asked-for events can ever come. Lockdep is the only oracle for the callback's
rule; doc/Notes.md has all five perturbations.

T23 added `KORU_OP_STAT`, the first opcode whose result is data rather than a
number: `res` plus `extra` is sixteen bytes and a `kstat` is about a hundred and
fifty, so `KoruStat` goes in the slot. `len` is the caller's buffer size and its
version negotiation, `off` must be eight-aligned, and `extra` carries the mask
of fields the kernel filled — the first non-zero `extra` koru has ever posted,
which is why the fuzzer's `extra` oracle is per-opcode from here. `OpWork` now
carries the submitter's `ARef<Credential>`, because a uid translated in a
kworker would be translated in init's namespace. That forced `CONFIG_USER_NS=y`
into `scripts/koru-debug.config`: with it off, `from_kgid` is unreachable from
Rust and the namespace handling cannot be shown to be wrong. doc/Notes.md has
the nine perturbations, one of which passed until the mask was redesigned.

T24 added the path operations `TRUNCATE`, `UTIMES` and `READLINK`, and
`kernel/koru_path.rs` with them: `kern_path` and the `LOOKUP_*` flags are
missing from `bindings::` and `do_delayed_call` is a static inline, so that file
holds every hand-declared prototype, each quoting its header. **Nothing checks
it** — re-read it on every kernel bump. All three ops are inline for `OPEN`'s
creds reason, and deferring one to the workqueue is now a regression test rather
than an argument. Every path op names its path the way `OPEN` does, with any
argument following it in the slot 8-aligned; the plan's `off`-is-the-length idea
was not taken. `Lookup` and `Link` are `Drop` guards for `path_put` and
`do_delayed_call`; the `mnt_want_write` guard the plan asked for is not needed
until T26. doc/Notes.md has the seven perturbations and why kmemleak is the
wrong instrument for the one leak here.

T25 added `KORU_OP_STATX_AT`, `STAT`'s answer for a path. It shares one
`getattr` helper with `STAT` and is inline for `OPEN`'s reason, so it needs no
carried cred. The struct **replaces the path it was given** at `off`, as
`READLINK`'s target does, which is why `off` must be 8-aligned and the whole
256 bytes must fit in the rest of the slot; there is no `len` negotiation here,
because `len` is the path's length. `dispatch` now returns `(res, extra)`: this
is the first inline op with anything to put in `extra`. doc/Notes.md has the
seven perturbations.

T26 added `KORU_OP_MKDIR` and `KORU_OP_SYMLINK`, the first ops that create
something. Both are `start_creating_path`, a `vfs_*` call and
`end_creating_path`, wrapped in `koru_path.rs`'s `Creating` guard — which holds
the parent's lock, the mount's write count and the path all at once, so the
`mnt_want_write` guard the plan asked for is **still** not needed. `vfs_mkdir`
may hand back a different dentry and it is that one the unlock belongs to.
`MKDIR`'s mode rides in `handle`, masked to `0o1777`; `SYMLINK` takes two
NUL-separated paths in one slot, target first. The heavy phase mounts its own
tmpfs and remounts it read-only, which is the only instrument that sees an
unbalanced write count. T26 also found that **the fuzzer's path sandbox was
never real** — one shared path slot per thread meant a batched `MKDIR` could
name a truncated prefix of anything another SQE had written. doc/Notes.md has
the six perturbations and the sandbox rebuild.

T27 added `KORU_OP_UNLINK` and `KORU_OP_RMDIR`, the first path ops with no
wrapper to call: `start_removing_path` is not exported and its exported sibling
takes a `__user` name, so `filename_unlinkat`'s sequence is written out by hand.
`koru_path.rs` gained `Write`, `Dirop` and `Inode`; the `mnt_want_write` guard
the plan has asked for since T24 belongs here and nowhere earlier. koru splits
the path itself, so a last component that is empty, `.`, `..` or followed by a
separator is **`EINVAL`**, where the syscalls spread four different errnos over
those cases. A read-only *bind mount* is the only shape that tests the write
count: with the superblock read-only, `inode_permission` refuses on its own and
the guard can be missing with nothing saying so. doc/Notes.md has the six
perturbations.

T28 added `KORU_OP_RENAME`, the last path op and the only one that locks two
directories: `start_renaming`, `vfs_rename`, `end_renaming`, with T26's two-path
parse and T27's split on each half. **One** `mnt_want_write`, not the two the
plan asked for — after the cross-mount rejection there is only one mount.
`EXDEV` joined the errno table as `Kind::Unsupported`. A mount-level rule can
only be tested with **two mounts of one filesystem**: across two filesystems
`lock_rename` answers `EXDEV` on its own, so the first version of that test
passed with the check deleted. doc/Notes.md has the four perturbations.

T29 added `KORU_OP_READDIR` and `KoruDirent`, the last kernel opcode and the
only one whose work happens in a callback the VFS makes into us. That callback
runs under the directory's `i_rwsem`, so it allocates nothing, takes no koru
lock and cannot panic; the arena is written after `iterate_dir` returns.
**`READDIR` is the first opcode that mutates shared per-file state** — `f_pos` —
so it is serialised per handle, and `OpWork::held_handle` sits beside
`held_slot`. A record's cookie means "resume *after* this entry", which is what
`linux_dirent64::d_off` means and needs the same back-patching. The plan's
deliberate breakage — the arena mutex inside the callback — turns out to be
benign on its own: the cycle needs T10's bug beside it. doc/Notes.md has the
five perturbations and that finding.

T30 built `rust/runtime/src/ops.rs`, Braam's operation layer: the twenty-five
calls of its `src/proc/io.h` that this substrate carries, signatures unchanged.
Four are koru's rather than Braam's and doc/Notes.md says why each is honest —
`seek_fd` is an assignment to a number userspace owns, `dup_fd` is a reference
count on one handle, `truncate_fd` goes by the path `open_at` recorded, and
`stat_of(path, false)` is an `lstat` composed out of `READLINK`. The done test
is `rust/runtime/tests/ops.rs`: a never-called `async fn` carrying every Braam
prototype, so a drift is a compile error, plus nineteen tests against libc on a
fixture tree.

T30 needed `OPEN` to create, so it is also the first kernel change since T29.
`KORU_O_CREAT`, `KORU_O_EXCL`, `KORU_O_TRUNC` and `KORU_O_APPEND` joined
`KORU_OPEN_FLAGS_ALL`, and **the creation mode rides after the path in the
slot**, where every path op has put its argument since T24 — which is the
placement `OPEN` itself originated. It also found that the fuzzer's *hostile*
generator could mutate an `OPEN`'s flags into a create-and-truncate on a path
outside the sandbox, and that five slot-exclusivity tests across both suites
were passing on luck.

T14 made the two userspace ABI mirrors a diff rather than a promise.
`cpp/include/koru_abi.h` and `cpp/include/koru_errno.h` are the C mirrors,
shared with `test/`, and an `abi_dump` on each side emits a canonical record
dump that `scripts/abi.sh` compares. That is the repo's first CMake build and
its fastest gate: no VM, no device, no module.

- `doc/Notes.md` — the global picture: design, ABI invariants, research
  findings, accepted gaps, and what T0–T12 established. It records *why* several
  obvious-looking approaches are wrong. Read it before writing anything.
- `doc/Plan.md` — the remaining tasks only, each with a "done" test. Completed
  tasks are deleted from it, not marked.
- `README.md` — the short explanation of the idea.
- `kernel/` — the out-of-tree Rust module. `koru_abi.rs` is the canonical wire
  format; `koru.rs` holds the device, the ring state and the `ENTER` path,
  `koru_ops.rs` holds opcode dispatch, the op implementations and `OpWork`, and
  `koru_path.rs` holds the hand-declared C prototypes and the `Drop` guards the
  path operations need.
- `test/` — `koru_check`, the one integrated test for the module. It includes
  the C ABI mirror from `cpp/include/` and keeps no copy. See Commands.
- `rust/` — the Cargo workspace. **Directories are named by role and packages
  by name**: `rust/sys` is `koru-sys`, the raw binding; `rust/runtime` is
  `koru` itself, holding the futures, the executor and the Braam surface
  (`vocab.rs`, `rt.rs`, `ops.rs`, `args.rs`, and `tz.rs`, which exists only
  because `clock_now` has no opcode and no std API); `rust/macros` is
  `koru-macros`, `#[koru::main]` alone. Cargo names the package, so `-p
  koru-sys` and `use koru_sys::` are unaffected by the directory. The examples
  in `rust/runtime/examples/` are programs, so the guest runs them.
- `cpp/` — the C ABI mirrors and `abi_dump`, built by the top-level
  `CMakeLists.txt`. The binding itself arrives at T39.
- `scripts/` — the guest-side check and the host-side runner that boots the VM,
  plus the dev kernel's config fragment. It has its own README.

Work proceeds in plan order. Task numbers are referenced across all three
documents; if you renumber, fix the cross-references.

**Licensing is split.** `kernel/` is GPL-2.0, because the module uses GPL-only
symbols and declares `MODULE_LICENSE("GPL")`. Everything else is MIT: the
bindings, `test/` and `scripts/`. A new file gets the SPDX tag of the half it
belongs to, and a userspace file must never be given GPL-2.0 by reflex — the
whole point is that a koru program is not obliged to be GPL.

Two documentation rules, both load-bearing:

- Every `*.md` file in this repo wraps at 80 columns and uses **no tables**.
  Reflow the whole paragraph when you edit one, rather than letting a line run
  long. A table forces long lines, so use headed sections or lists instead.
- When a task is finished, **delete it from `doc/Plan.md`**. Anything it taught
  that outlives it — a corrected assumption, an ABI change, a trap worth not
  falling into twice — moves into `doc/Notes.md` first. `Plan.md` is future
  work only; `Notes.md` is the accumulated global picture.

## What this project is

`koru`: an experimental non-POSIX Linux kernel API designed for coroutines.
Submission and completion are separate events (a queue, not a trap), because a
coroutine must yield to its executor between the call and the answer. An
out-of-tree Rust kernel module provides `/dev/koru`; two independent userspace
bindings — Rust `Future`s and C++20 awaiters — sit on the same unchanged ABI.

## Development environment

The host is Debian forky/sid on bare metal (Apple iMac19,1, i9-9900K, 16
threads). It builds everything, but **never load a development module on the
host kernel** — module bugs panic the machine, and this is somebody's desktop,
not an expendable box. All module loading happens in a VM (see below).

T0 is done. The dev kernel lives outside this repo at `../kernel-dev/`:

- `../kernel-dev/linux-source-7.1/` — Debian `linux-source-7.1` (7.1.12, same
  version as the host's running kernel), configured and built. Keep the whole
  tree: `rust/*.rmeta` is only there, and the installed `linux-headers-7.1.12`
  genuinely has no `rust/` directory, so out-of-tree Rust modules cannot be
  built against it.

Only the tree lives there. The config fragment that built it is
`scripts/koru-debug.config`, under version control with the scripts: an earlier
copy outside the repo drifted unnoticed for four commits after a rename.

The base config comes from `vng --kconfig`, so it is a small VM-only kernel; a
full build takes about 9 minutes and the tree is ~5.4 GB. After any config
change, re-check that the options actually survived `olddefconfig` —
`merge_config.sh` drops unmet ones silently.

Not every fragment option is debug instrumentation. T23 added
`CONFIG_USER_NS=y`, which the module now **requires**: without it `from_kgid` is
a static inline bindgen never emits, so `STAT` cannot translate a gid at all.
A config change means rebuilding the kernel and then the module against it.

Verified toolchain: rustc and cargo 1.98.1 from **rustup**, with the `rust-src`
component, plus bindgen 0.72.1, clang and lld 21 from Debian testing, and
`make LLVM=1`. `make LLVM=1 rustavailable` passes. Floors from the design were
rustc 1.85.0 and bindgen 0.71.1. For C++: GCC ≥ 11 or Clang ≥ 14, `-std=c++20`,
built with `-fsanitize=address,undefined`.

T13 moved the whole project off Debian's rustc 1.95.0 onto rustup's 1.98.1;
`~/.cargo/env` puts it ahead of `/usr/bin`. Kernel Rust needs `rust-src`, so
`rustup component add rust-src` is not optional. One config option changed:
`RUSTC_CLANG_LLVM_COMPATIBLE` is gone, because rustc now carries LLVM 22 against
clang's 21. It gates only `RUST_INLINE_HELPERS`, which was never on. After any
toolchain move, re-run `olddefconfig` and **re-check the fragment options
survived**.

## Commands

These work today (T0 through T30):

```sh
KDIR=../kernel-dev/linux-source-7.1

# Rebuild the dev kernel after a config or source change.
make -C $KDIR LLVM=1 -j16 && make -C $KDIR LLVM=1 -j16 modules

# Build the module, from kernel/. LLVM=1 is not optional: the dev kernel is
# built with clang and lld. Output lands in kernel/ and is gitignored.
make -C $KDIR M=$PWD LLVM=1

# Boot the dev kernel in a VM and run a command. virtme-ng mounts the host
# filesystem read-only under a tmpfs overlay, so there is no disk image, a
# panic costs nothing, and this repo is visible at its usual path inside the
# guest. Drop --exec for an interactive shell.
vng --run $KDIR --user root --memory 4G --cpus 4 --exec "<command>"

# The test binary, built on the host and run in the guest.
make -C test

# The whole check: one VM boot, about 27 seconds, one verdict line.
scripts/run.sh
scripts/run.sh open read cancel   # just those sections
KORU_SEED=12345 scripts/run.sh    # replay a fuzz failure

# The Rust binding. The library half needs no device and runs on the host in
# under a second; the integration suite needs /dev/koru, so it runs in a VM.
(cd rust && cargo fmt --all -- --check)
(cd rust && cargo test -p koru-sys --lib)      # ABI, ioctl numbers, errnos
(cd rust && cargo test -p koru --lib)          # cookie, slab, ops, vocab, tz
(cd rust && cargo test --workspace --no-run)   # build before the runner
(cd rust && cargo build --examples)            # the programs the runner runs
scripts/run-rust.sh                            # kernel, runtime and ops suites
scripts/run-rust.sh cancel read                # only matching test names
KORU_SEED=12345 scripts/run-rust.sh            # replay a race loop
KORU_ITERS=100000 TIMEOUT=2400 scripts/run-rust.sh drop_safety

# The ABI conformance diff. No VM, no device, no module: the fastest gate
# there is. cmake -B build once, then rebuild whenever a mirror changes.
cmake -B build && cmake --build build
scripts/abi.sh
ctest --test-dir build          # the same comparison, as a registered test
```

`test/koru_check` is the entire test suite for the module: one binary, one
shared ring, one process, plus the two `rmmod` races that need a second one.
`kernel/koru_abi.rs` is canonical and is copied into two userspace mirrors,
`rust/sys/src/abi.rs` and `cpp/include/koru_abi.h`, which `test/` includes
rather than copying again. **A wire-format change is all three files**, and
only the kernel one is unchecked: `scripts/abi.sh` diffs the other two, and the
header's own `static_assert`s catch a layout slip at compile time. The C++
suite comes at T39.

`scripts/rust.sh` runs all three Rust suites in one VM boot:
`rust/sys/tests/kernel.rs`, which is T4–T11 plus the T3 matrices;
`rust/runtime/tests/runtime.rs`, which is the futures and the executor; and
`rust/runtime/tests/ops.rs`, which is Braam's prototypes and the operation layer
against libc. None supersedes `koru_check`, which keeps the fuzz, the two
`rmmod` races and the heavy-phase leak window. Two gates exist because `#[test]`
can pass without
proving anything: a filter matching nothing exits 0, so `rust.sh` asserts a
minimum passed count **per suite**, taken from the `SUITES` list in
`scripts/run-rust.sh` and raised when a test is added; and a skipped
precondition prints `KORU-RS-SKIP`, which fails the run.

**Section order in `koru_check` is load-bearing.** Everything that allocates in
bulk runs first and is marked `heavy` in the table in `koru_check.c`; the binary
stamps `KORU-HEAVY-END-MS` and the script sleeps only the shortfall below
kmemleak's five-second minimum object age. A bulk-allocating loop added to the
tail is invisible to the one leak scan, and nothing will say so.

`ENTER` must never touch a `UserSlice` while holding the ring `SpinLock`:
`copy_*_user` can fault and therefore sleep. The submit and reap loops are
structured around that, and `DEBUG_ATOMIC_SLEEP` plus lockdep in the dev kernel
is what catches a slip. The submitter `Mutex` is likewise dropped around the
`CondVar` wait, or one waiter blocks every submitter.

`ENTER` must not sleep when `min_complete` is unreachable, and unreachable means
`inflight == 0` — not `len == 0 && inflight == 0`. A caller asking for more than
is queued with nothing running slept for ever until T11 found it.

Wait semantics: `min_complete` decides whether `ENTER` waits at all,
`timeout_ns` only caps the wait and 0 means no cap. `ENTER` returns SQEs
consumed on success and `-EINTR` on a signal; either way `submitted` and
`completed` are written back, so an interrupted call still says what not to
resubmit.

**The arena.** `slot_size` must be a multiple of `PAGE_SIZE`, so every slot is a
whole number of pages and nothing straddles a page boundary. Pages are allocated
and zeroed at `SETUP`, not at `mmap`, so `mmap` never allocates and `SETUP`
cannot promise memory it has not got. `mmap` is one-shot and demands
`MAP_SHARED`, `vm_pgoff == 0` and a length exactly equal to `arena_size`;
`MAP_PRIVATE` would silently give copy-on-write and is the failure doc/Notes.md
singles out.

`VM_IO` is deliberately **not** set. The `set_dontcopy` doc says `VM_DONTCOPY`
is only permanent with `VM_IO`, but `MADV_DOFORK` actually refuses on
`VM_SPECIAL`, which includes `VM_MIXEDMAP` and `VM_DONTEXPAND`. Both are already
set, so userspace cannot undo it, and there is a test asserting the `madvise`
fails.

**Slot exclusivity is kernel-enforced**, by a bitmap in `RingState`. A slot is
claimed in ioctl context after validation and released in the same critical
section that posts the CQE, so it is free exactly when userspace can see the
completion. An op that is refused a slot gets `-EBUSY` as its completion and
must not release the slot the winner holds. **`CHECKSUM` is deferred to the
workqueue**, not run inline: an inline op holds its slot only inside the submit
loop, so a collision could never happen and the rule would be untestable. `READ`
takes the same shape at T10.

**READ locking.** The arena `Mutex` is taken inside `mmap`, which the VFS calls
under `mmap_lock`, and a filesystem read takes `mmap_lock` under the inode
rwsem. So **nothing may hold the arena mutex across a call that can reach the
VFS** — `do_read` holds it only around each `write_raw`. Holding it across
`kernel_read` is a real deadlock and lockdep catches it.

`READ` and `OPEN` accept regular files only (`OPEN` also takes directories). A
blocking read in a kworker cannot be interrupted, so a FIFO or socket would
consume a workqueue thread permanently. `check_readable` also rejects a file
whose `f_op` has `read` set or `read_iter` unset, which is what keeps
`kernel read not supported for file` out of the log.

**`release` cancels queued work**, so `close(fd)` frees the ring instead of
leaving it pinned for the length of the longest delay. It uses the same refcount
rule as `CANCEL` and posts no completions, because nothing can still be reading
the CQ. The check asserts the timing, not just the outcome.

**`slot_try_acquire` does not bounds-check its own index.** Every caller must
validate `slot < slot_count` first; forgetting it is a Rust bounds panic and a
kernel Oops, reachable from userspace. The bitmap is whole 64-bit words, so a
small `slot_count` leaves spare bits and `slot_count + 1` stays in bounds, so
probe past 64 when testing this.

**`CANCEL`'s refcount rule is the sharpest edge in the module.**
`enqueue_delayed` returning `Ok` leaks one `Arc` reference that only `run()`
reclaims. So drop exactly one reference **if and only if**
`cancel_delayed_work` returned `true`: skipping it leaks the op and wedges
`rmmod`, and doing it on a `false` return is an immediate use-after-free. Both
were demonstrated by breaking them.

In-flight deferred ops live in `RingCtx::pending`, a `Mutex<KVec<Arc<OpWork>>>`.
That is a reference cycle broken by unregistering on every completion path, and
`run()` unregisters **after** `complete()` so a cancel during execution reports
`-EALREADY` rather than `-ENOENT`. That window is narrow but real: the check's
64 KB whole-slot `CHECKSUM` reaches it one to three times per thousand rounds,
which is what makes the cancel refcount rule testable in both directions.

**A deferred op owns everything it needs**, resolved at submit time: the `Sqe`
by value, an `Arc<RingCtx>`, an `ARef<File>` and an `ARef<Credential>`. Never an
index into a table that can be reindexed, and never a pointer into the arena.
The creds are the submitter's: in a kworker `current_cred()` is `init_cred` and
`current_user_ns()` is init's, so `STAT` would report container-wrong ids.

**An opcode that holds its slot across a deferred window must be in
`OpWork::held_slot`** — `CHECKSUM`, `READ`, `WRITE`, `STAT`. Forgetting it
leaves the slot busy for ever after a cancel, and nothing says why.

**Handles.** A handle is `(index: u16, generation: u16)`, index low, generation
starting at 1 and skipping 0 on wrap — so a valid handle is never 0 and every
other opcode can keep demanding a zero `handle` field. `CLOSE` bumps the
generation, which is what makes a reused index reject the retired handle. The
table is fixed-size, sized at `SETUP` from `handle_count`, and guarded by a
`Mutex`, never a `SpinLock`, because `fput` sleeps. `release` drains it
explicitly: an in-flight `OpWork` holds its own `Arc<RingCtx>`, so dropping the
`Arc` alone would not free anything.

`OPEN` carries its flags in the SQE's `handle` field, using koru's own
`KORU_O_*` bit values rather than the host `O_*` constants. Add a flag by
whitelisting it in `KORU_OPEN_FLAGS_ALL` and translating it; an unlisted bit
must stay `-EINVAL`. **`KORU_O_CREAT` means the slot also carries a `u64`
creation mode after the path**, at `arg_offset(off, len)`, where every path op
has put its argument since T24 — so an `OPEN` that does not create needs no room
for one, and there is a test for a path at the very end of a slot. The mode is
masked to `KORU_OPEN_MODE_ALL`, which is `MKDIR`'s `0o1777`: koru creates
nothing setuid and says so rather than dropping the bits.

**kmemleak cannot see a leaked handle** — the file stays referenced by our own
table. Use field 1 of `/proc/sys/fs/file-nr`. `/proc/<pid>/fd` sees nothing
either way, since `filp_open` installs no descriptor.

**The module pins itself.** `kernel::miscdevice` leaves `fops.owner` NULL, so
neither an open fd nor a queued work item pins the module on its own.
`open`/`release` and the deferred op path take and drop explicit references,
which makes `rmmod` return `-EBUSY` rather than free text still in use. Every
new path that outlives an `ENTER` needs the same treatment, and a missing
`module_put` wedges the module as permanently unloadable.

A test that can hang must arm an alarm and be line-buffered. `_exit` from the
handler drops a full stdout buffer, which turns a diagnosable hang into a silent
one.

**The check must fail on a kernel splat, not just print it.** The dmesg scan in
`scripts/check.sh` captures its grep into a variable and the verdict gates on
that being empty. T10's lockdep deadlock first reported `PASS` with the cycle
printed right above the pass line, because the check was advisory. The pattern
also matches `not supported for file`, a `pr_warn_ratelimited` rather than a
`WARN_ON`. It excludes the script's own `/dev/kmsg` fences, which otherwise
match their own pattern.

`scripts/check.sh` carries every pass condition there is. Keep it small and
obvious: a bug in it weakens the whole verdict at once.

**A test that depends on losing a race has to loop until it wins.** Five
slot-exclusivity tests asserted that an op behind a deferred one gets `-EBUSY`
on the first try, and passed for twelve tasks because a whole-slot `CHECKSUM`
usually outlives the submit loop. Usually, not always: adding a test ahead of
them made the Rust suite fail one run in ten. Both suites now round the pair 64
times and assert the window is reached at least once — `Mapped::slot_race` in
`rust/sys/tests/common/mod.rs` and `race_round` in `test/koru_test.c`. Write a
new one that way from the start.

**After restoring a perturbed source file by hand, `touch` it.** `mv` keeps the
old mtime, `make` then skips the rebuild, and the guest runs the *previous*
perturbation's module. It presents as a test that passes alone and fails in the
full run.

Assert the exact errno, never just that a call failed. The T3 dispatcher
returned `EPROTO` where it owed `ENOTTY`, and only an exact-errno assertion
noticed. Every rejection test was then confirmed to have teeth by deleting the
corresponding kernel check and watching that one test, and only that one, fail.

**kmemleak reports nothing about an object younger than five seconds**
(`MSECS_MIN_AGE` in `mm/kmemleak.c`). Scanning right after a test loop reports a
clean result no matter how badly the code leaks. `koru_check` runs its
bulk-allocating sections first and stamps the time; `scripts/check.sh` measures
the age of that stamp and sleeps only the shortfall, then scans twice, because
the first pass after heavy allocation is not settled. Treat any new leak test as
untrustworthy until it has been shown to fail on a real leak: an `Arc` leaked
per open is invisible here, because our own table still references the file.
Use `/proc/sys/fs/file-nr` for that, and an unreachable allocation to test the
scan itself.

Expected taint with the module loaded is exactly 4096, `TAINT_OOT_MODULE`.
Module signing is off in this kernel, so bit 13 must never appear; anything
other than 4096 is a finding.

Only `koru.rs` is named in `kernel/Kbuild`. The other kernel `.rs` files are
submodules of that one crate, reached by `mod` declarations, not separate
`obj-m` entries. This is verified working, including rebuilds triggered by
editing a submodule alone. Inherent `impl RingCtx` blocks live in `koru_ops.rs`
too, which is why `RingCtx` and its fields are `pub(crate)` rather than private.

The rest of `doc/Plan.md`'s Verification sequence does not work yet; the
load-bearing one will be:

```sh
cargo run --example read_file   # Rust demo
```

Add them here as they start working, rather than inventing them ahead of time.

## Architecture

Three planes, deliberately separated:

- **Control plane — the `ENTER` ioctl.** Submissions in, completions out, via
  `UserSlice` (`copy_from_user`). This is the *only* entry point. It also blocks
  for completions.
- **Data plane — the `mmap`'d arena.** Fixed-size slots in order-0 pages the
  kernel allocated and owns, inserted with `vm_insert_page`.
- **Dispatch — opcode table plus workqueue.** Fast ops complete inline; blocking
  ops go to worker threads.

The coroutines live entirely in userspace. Kernel Rust has no async executor, no
`Future`, no `kasync` — this is permanent, not a temporary shortcut, and the
kernel side is a dispatch table plus a worker pool (the same split io_uring
makes with `io-wq`).

## Invariants that constrain every change

These are load-bearing. Breaking one is a security bug or an unfixable ABI wart,
not a style question.

- **The kernel never dereferences a userspace address.** Operations name buffers
  by *slot index*, never pointer. This is what makes use-after-free
  unrepresentable rather than merely prevented, and it is what lets the C++
  binding be safe without a borrow checker.
- **C1 — every consumed SQE produces exactly one CQE.** Including malformed
  SQEs, unknown opcodes and cancelled ops. io_uring violates this
  (`sq_dropped`); do not copy it.
- **E1 — a bad SQE never fails the `ENTER` ioctl.** It yields a CQE with
  `res = -EINVAL`. The ioctl return value reports protocol failures only, and on
  success is the count of SQEs consumed — never the completion count.
- **Reserved fields must be zero; unknown flag bits are rejected.** The only
  thing that permits adding fields later without breaking old binaries.
- **CQ overflow is prevented by admission control**, not an overflow list —
  reserve a CQ slot at SQE-consumption time. Consequence: no multishot ops
  without redesigning this.

## Things that look right and are not

Recorded so they don't get re-proposed. `doc/Notes.md` has the full reasoning
and citations.

- **Do not reach for `uring_cmd`.** It has no Rust abstraction in mainline —
  only an unmerged RFC with open soundness bugs.
- **Do not build a shared-memory SQ/CQ ring.** `Page`'s public accessors
  (`read_raw`, `write_raw`) are `memcpy` with a documented "no concurrent
  access" precondition that a ring shared with untrusted userspace violates by
  definition, and there is no way to place an `Atomic<u32>` over a shared word.
  This is why the control plane is an ioctl. Revisiting it is T50, and it
  requires solving that problem first.
- **Do not defer `OPEN` to a workqueue.** In a kworker, `current_cred()` is
  `&init_cred` and `current->fs` is the init root, so `filp_open` would resolve
  and permission-check as root in the initial namespaces — a privilege
  escalation for anyone who can open the device. `OPEN` runs inline in the
  submitting task's context. Handles resolve to `ARef<File>` at submit time, in
  ioctl context, for the same reason.
- **`index < N` is not sufficient validation** for a buffer slot. Nothing stops
  userspace putting the same index in two concurrent SQEs. Slot exclusivity is
  kernel-enforced (`slot_busy` bitmap, `-EBUSY` on collision); the userspace
  pool is advisory.
- **`ENTER` must use `CondVar::wait_interruptible_timeout`.** Plain `wait()`
  leaves the process unkillable in D-state with an unloadable module.

Expect a substantial fraction of the kernel work to be raw `bindings::` +
`unsafe`: `filp_open`, `kernel_read` and `override_creds` have no safe Rust
wrappers.

## Cross-language ABI

`kernel/koru_abi.rs` is canonical; `rust/sys/src/abi.rs` and
`cpp/include/koru_abi.h` mirror it, and `cpp/include/koru_errno.h` mirrors
`rust/sys/src/error.rs`. The two mirrors are kept in step by
`scripts/abi.sh`, which diffs an `abi_dump` emitted by each side. Touch any of
them and run it — then confirm it actually fails when you perturb a field, or
it proves nothing.

Adding a field or a constant means editing **both** emitters, and a record
missing from both diffs clean. That is why each struct declares its field count
and each emitter checks that the field sizes sum to `sizeof`; keep new structs
padding-free so that guard keeps working. The diff cannot see the kernel at
all — the caps assertions in `rust/sys/tests/kernel.rs` are the only thing
tying a mirror to the canonical file, and they run in the VM.

The two userspace bindings share no code. That is intentional: the second
binding exists to demonstrate the ABI is language-neutral, so resist factoring
common logic across them.
