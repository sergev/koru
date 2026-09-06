# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## State of the repository

**Design only — no code exists yet.** The repo holds two documents:

- `Plan.md` — canonical design plus a task list T0–T23, each with a "done" test. Read it
  before writing anything. It records *why* several obvious-looking approaches are wrong.
- `README.md` — the short explanation of the idea.

Work proceeds in plan order. Task numbers are referenced across both documents; if you
renumber, fix the cross-references.

## What this project is

`xring`: an experimental non-POSIX Linux kernel API designed for coroutines. Submission and
completion are separate events (a queue, not a trap), because a coroutine must yield to its
executor between the call and the answer. An out-of-tree Rust kernel module provides
`/dev/xring`; two independent userspace bindings — Rust `Future`s and C++20 awaiters — sit
on the same unchanged ABI.

## Development environment

The host here is macOS and **cannot build or run any of this**. All kernel work happens on
a separate Linux machine, which needs a kernel ≥6.16 built with `CONFIG_RUST=y` *and its
full build tree retained* — distro `linux-headers` packages omit `rust/*.rmeta` and cannot
build out-of-tree Rust modules. Build the dev kernel with KASAN, `PROVE_LOCKING` and
`DEBUG_KMEMLEAK` on. Use a VM; module bugs panic the machine.

Toolchain floors: rustc 1.85.0, bindgen 0.71.1, `make LLVM=1`. For C++: GCC ≥ 11 or
Clang ≥ 14, `-std=c++20`, built with `-fsanitize=address,undefined`.

## Commands

None work yet. `Plan.md`'s Verification section defines the intended end-to-end sequence;
the load-bearing ones will be:

```sh
make -C <kernel-tree> M=$PWD LLVM=1 && insmod xring.ko
cargo test -p xring-sys                  # Rust ABI + per-opcode integration tests
cargo run --example read_file            # Rust demo
ctest --test-dir build                   # C++ tests, under ASan+UBSan
diff <(./build/abi_dump) <(cargo run -q --bin abi_dump)   # must be empty
```

Add them here as they start working, rather than inventing them ahead of time.

## Architecture

Three planes, deliberately separated:

- **Control plane — the `ENTER` ioctl.** Submissions in, completions out, via `UserSlice`
  (`copy_from_user`). This is the *only* entry point. It also blocks for completions.
- **Data plane — the `mmap`'d arena.** Fixed-size slots in order-0 pages the kernel
  allocated and owns, inserted with `vm_insert_page`.
- **Dispatch — opcode table plus workqueue.** Fast ops complete inline; blocking ops go to
  worker threads.

The coroutines live entirely in userspace. Kernel Rust has no async executor, no `Future`,
no `kasync` — this is permanent, not a temporary shortcut, and the kernel side is a
dispatch table plus a worker pool (the same split io_uring makes with `io-wq`).

## Invariants that constrain every change

These are load-bearing. Breaking one is a security bug or an unfixable ABI wart, not a
style question.

- **The kernel never dereferences a userspace address.** Operations name buffers by *slot
  index*, never pointer. This is what makes use-after-free unrepresentable rather than
  merely prevented, and it is what lets the C++ binding be safe without a borrow checker.
- **C1 — every consumed SQE produces exactly one CQE.** Including malformed SQEs, unknown
  opcodes and cancelled ops. io_uring violates this (`sq_dropped`); do not copy it.
- **E1 — a bad SQE never fails the `ENTER` ioctl.** It yields a CQE with `res = -EINVAL`.
  The ioctl return value reports protocol failures only, and on success is the count of
  SQEs consumed — never the completion count.
- **Reserved fields must be zero; unknown flag bits are rejected.** The only thing that
  permits adding fields later without breaking old binaries.
- **CQ overflow is prevented by admission control**, not an overflow list — reserve a CQ
  slot at SQE-consumption time. Consequence: no multishot ops without redesigning this.

## Things that look right and are not

Recorded so they don't get re-proposed. `Plan.md` has the full reasoning and citations.

- **Do not reach for `uring_cmd`.** It has no Rust abstraction in mainline — only an
  unmerged RFC with open soundness bugs.
- **Do not build a shared-memory SQ/CQ ring.** `Page`'s public accessors (`read_raw`,
  `write_raw`) are `memcpy` with a documented "no concurrent access" precondition that a
  ring shared with untrusted userspace violates by definition, and there is no way to place
  an `Atomic<u32>` over a shared word. This is why the control plane is an ioctl. Revisiting
  it is T22, and it requires solving that problem first.
- **Do not defer `OPEN` to a workqueue.** In a kworker, `current_cred()` is `&init_cred` and
  `current->fs` is the init root, so `filp_open` would resolve and permission-check as root
  in the initial namespaces — a privilege escalation for anyone who can open the device.
  `OPEN` runs inline in the submitting task's context. Handles resolve to `ARef<File>` at
  submit time, in ioctl context, for the same reason.
- **`index < N` is not sufficient validation** for a buffer slot. Nothing stops userspace
  putting the same index in two concurrent SQEs. Slot exclusivity is kernel-enforced
  (`slot_busy` bitmap, `-EBUSY` on collision); the userspace pool is advisory.
- **`ENTER` must use `CondVar::wait_interruptible_timeout`.** Plain `wait()` leaves the
  process unkillable in D-state with an unloadable module.

Expect a substantial fraction of the kernel work to be raw `bindings::` + `unsafe`:
`filp_open`, `kernel_read` and `override_creds` have no safe Rust wrappers.

## Cross-language ABI

`kernel/xring_abi.rs` is canonical; `user/cpp/include/xring_abi.h` mirrors it. They are kept
identical by a conformance test (T16) that diffs an `abi_dump` emitted by each side. When
you touch either file, run that diff — and confirm the test actually fails when you perturb
a field, or it proves nothing.

The two userspace bindings share no code. That is intentional: the second binding exists to
demonstrate the ABI is language-neutral, so resist factoring common logic across them.
