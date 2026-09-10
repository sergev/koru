# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

## State of the repository

**T0–T8 are done.** The module registers `/dev/koru`, configures a ring with
`SETUP`, and submits `NOP`, `DELAY_NS` and `CHECKSUM` through `ENTER`, which
blocks for completions. The arena is mmap'd, with slot exclusivity enforced by
the kernel. Teardown is safe and `rmmod` is refused while anything is live.

- `doc/Notes.md` — the global picture: design, ABI invariants, research
  findings, accepted gaps, and what T0–T8 established. It records *why* several
  obvious-looking approaches are wrong. Read it before writing anything.
- `doc/Plan.md` — the remaining tasks only, each with a "done" test. Completed
  tasks are deleted from it, not marked.
- `README.md` — the short explanation of the idea.
- `kernel/` — the out-of-tree Rust module. `koru_abi.rs` is the canonical wire
  format.
- `test/` — interim C tests, one per task. See Commands.

Work proceeds in plan order. Task numbers are referenced across all three
documents; if you renumber, fix the cross-references.

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
- `../kernel-dev/koru-debug.config` — the debug fragment: KASAN
  generic+inline+vmalloc, `PROVE_LOCKING`, `DEBUG_KMEMLEAK`, `DEBUG_OBJECTS`,
  DWARF5, `SAMPLE_RUST_MINIMAL=m`. `MODVERSIONS`, `MODULE_SIG`, `DEBUG_INFO_BTF`
  and `RANDSTRUCT` are deliberately off.
- `../kernel-dev/t0-donetest.sh` — the T0 done test, run inside the guest.

The base config comes from `vng --kconfig`, so it is a small VM-only kernel; a
full build takes about 9 minutes and the tree is ~5.4 GB. After any config
change, re-check that the options actually survived `olddefconfig` —
`merge_config.sh` drops unmet ones silently.

Verified toolchain (all from Debian testing): rustc 1.95.0 with rust-src,
bindgen 0.72.1, clang and lld 21, `make LLVM=1`. `make LLVM=1 rustavailable`
passes. Floors from the design were rustc 1.85.0 and bindgen 0.71.1. For C++:
GCC ≥ 11 or Clang ≥ 14, `-std=c++20`, built with `-fsanitize=address,undefined`.

## Commands

These work today (T0 through T8):

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

# Done tests.
vng --run $KDIR --user root --memory 4G --cpus 4 \
    --exec "sh ../kernel-dev/t0-donetest.sh"      # kernel itself
vng --run $KDIR --user root --memory 4G --cpus 4 \
    --exec "sh ../kernel-dev/t1-donetest.sh"      # insmod/rmmod koru.ko
vng --run $KDIR --user root --memory 4G --cpus 4 \
    --exec "sh ../kernel-dev/t2-donetest.sh"      # 10k open/close, kmemleak
vng --run $KDIR --user root --memory 4G --cpus 4 \
    --exec "sh ../kernel-dev/t3-donetest.sh"      # SETUP / GET_PARAMS
vng --run $KDIR --user root --memory 4G --cpus 4 \
    --exec "sh ../kernel-dev/t4-donetest.sh"      # ENTER, NOP
vng --run $KDIR --user root --memory 4G --cpus 4 \
    --exec "sh ../kernel-dev/t5-donetest.sh"      # DELAY_NS, blocking wait
vng --run $KDIR --user root --memory 4G --cpus 4 \
    --exec "sh ../kernel-dev/t6-donetest.sh"      # teardown, module pinning
vng --run $KDIR --user root --memory 4G --cpus 4 \
    --exec "sh ../kernel-dev/t7-donetest.sh"      # arena mmap, CHECKSUM
vng --run $KDIR --user root --memory 4G --cpus 4 \
    --exec "sh ../kernel-dev/t8-donetest.sh"      # slot exclusivity

# Interim userspace tests, built on the host and run in the guest.
make -C test
```

`test/` holds a small C program per task plus a shared harness in `koru_test.h`
and `koru_test.c`. It is scaffolding: the real suites are the Rust one at T13
and the C++ one at T17. The header's first section is a hand-written mirror of
`kernel/koru_abi.rs`, so **a change there means a matching change in that one
place**, and its `_Static_assert`s are what catch you forgetting. T16 deletes
that section in favour of including the real `user/cpp/include/koru_abi.h`.

`ENTER` must never touch a `UserSlice` while holding the ring `SpinLock`:
`copy_*_user` can fault and therefore sleep. The submit and reap loops are
structured around that, and `DEBUG_ATOMIC_SLEEP` plus lockdep in the dev kernel
is what catches a slip. The submitter `Mutex` is likewise dropped around the
`CondVar` wait, or one waiter blocks every submitter.

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

**The module pins itself.** `kernel::miscdevice` leaves `fops.owner` NULL, so
neither an open fd nor a queued work item pins the module on its own.
`open`/`release` and the deferred op path take and drop explicit references,
which makes `rmmod` return `-EBUSY` rather than free text still in use. Every
new path that outlives an `ENTER` needs the same treatment, and a missing
`module_put` wedges the module as permanently unloadable.

A test that can hang must arm an alarm and be line-buffered. `_exit` from the
handler drops a full stdout buffer, which turns a diagnosable hang into a silent
one.

Assert the exact errno, never just that a call failed. The T3 dispatcher
returned `EPROTO` where it owed `ENOTTY`, and only an exact-errno assertion
noticed. Every rejection test was then confirmed to have teeth by deleting the
corresponding kernel check and watching that one test, and only that one, fail.

**kmemleak reports nothing about an object younger than five seconds**
(`MSECS_MIN_AGE` in `mm/kmemleak.c`). Scanning right after a test loop reports a
clean result no matter how badly the code leaks. Every leak check must sleep
past that age first; `t2-donetest.sh` sleeps 8 seconds, then scans twice,
because the first pass after heavy allocation is not settled. This was caught by
deliberately leaking an `Arc` per open and finding the check silent, so treat
any new leak test as untrustworthy until it has been shown to fail on a real
leak.

Expected taint with the module loaded is exactly 4096, `TAINT_OOT_MODULE`.
Module signing is off in this kernel, so bit 13 must never appear; anything
other than 4096 is a finding.

Only `koru.rs` is named in `kernel/Kbuild`. The other kernel `.rs` files are
submodules of that one crate, reached by `mod` declarations, not separate
`obj-m` entries. This is verified working, including rebuilds triggered by
editing a submodule alone.

The rest of `doc/Plan.md`'s Verification sequence does not work yet; the
load-bearing ones will be:

```sh
cargo test -p koru-sys          # Rust ABI + per-opcode integration tests
cargo run --example read_file   # Rust demo
ctest --test-dir build          # C++ tests, under ASan+UBSan
diff <(./build/abi_dump) <(cargo run -q --bin abi_dump)   # must be empty
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
  This is why the control plane is an ioctl. Revisiting it is T22, and it
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

`kernel/koru_abi.rs` is canonical; `user/cpp/include/koru_abi.h` mirrors it.
They are kept identical by a conformance test (T16) that diffs an `abi_dump`
emitted by each side. When you touch either file, run that diff — and confirm
the test actually fails when you perturb a field, or it proves nothing.

The two userspace bindings share no code. That is intentional: the second
binding exists to demonstrate the ABI is language-neutral, so resist factoring
common logic across them.
