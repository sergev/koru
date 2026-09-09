# xring — a coroutine-oriented, non-POSIX Linux kernel API (PoC)

## Context

POSIX's syscall shape is synchronous: a trap is a rendezvous, control leaves and returns
at the same point. That is fundamentally at odds with coroutines, where the caller must
yield to its executor between "call" and "return". This project designs a fresh kernel API
built for async/await from the start — no POSIX compatibility, no `errno`, no fds — and
proves it with a working userspace program driven entirely through the new interface.

The kernel side is an out-of-tree Rust module for dispatch. Two independent userspace
bindings sit on top of the same unchanged ABI: a Rust crate with `Future` impls, and a
C++20 header library with `co_await`-able awaiters. Each has its own executor.

The defining constraint, chosen deliberately: **a coroutine can never own kernel-visible
memory.** Rust futures can be dropped at any await point (`select!`, timeouts, cancelled
tasks), and `Drop` cannot be async — so any design where an in-flight operation holds a
userspace pointer has an unavoidable use-after-free. We remove the possibility rather than
manage it.

Outcome: `/dev/xring`, a Rust module implementing it, and two demos — one Rust, one
C++20 — that each open and read a real file through coroutines while timers complete out
of order.

## Research findings that shaped this design

Verified against mainline `rust/kernel/` and kernel docs. Four findings changed the
approach materially:

1. **`uring_cmd` has no Rust abstraction.** Only Sidong Yang's unmerged
   [RFC v3](https://lkml.org/lkml/2025/8/22/1054), with open soundness bugs. We build our
   own transport instead of layering on io_uring.
2. **A shared-memory SQ/CQ ring is not implementable with merged Rust APIs.**
   `Page::with_page_mapped` / `with_pointer_into_page` are private. The only public
   accessors — `read_raw`, `write_raw`, `fill_zero_raw` — are `memcpy` under a transient
   `kmap_local_page`, with a documented precondition of *no concurrent access*. A ring
   shared with untrusted userspace violates that by definition, and they give no way to
   place an `Atomic<u32>` over a shared word or to do an acquire-load. **Consequence: the
   control plane is an ioctl using `UserSlice` (`copy_from_user`), not shared memory.**
   Only the data-plane arena is mmap'd.
3. **Deferring `open()` to a workqueue is a root privilege escalation.** In a kworker,
   `current_cred()` is `&init_cred` and `current->fs` is the init root — `filp_open` would
   resolve and permission-check as root in the initial namespaces. io_uring handles this
   with `override_creds` plus io-wq workers that clone the ring creator's `mm`/`fs`/
   `nsproxy`; `kernel::cred::Credential` exposes no such wrapper. **Consequence: `OPEN`
   runs inline in the submitting task's context.**
4. **No async executor exists in kernel Rust.** No `Future`, no `kasync`;
   `kernel::workqueue` is callback-based. The coroutines live entirely in userspace; the
   kernel is a dispatch table plus a worker pool — the same split io_uring makes with
   `io-wq`. This is a permanent property of the design, not a temporary shortcut.

Merged and usable: `kernel::miscdevice` (6.13; `open`/`release`/`ioctl`/`mmap`/`read_iter`,
**no `poll`**), `kernel::mm::virt` (6.16; `VmaNew::set_mixedmap`,
`VmaMixedMap::vm_insert_page`), `kernel::sync::{CondVar, SpinLock, Mutex, Arc, atomic}`,
`kernel::workqueue` (`Work`, `DelayedWork`, `enqueue`, `enqueue_delayed` — but **no**
`cancel_work_sync` or `flush_workqueue`), `kernel::uaccess::UserSlice`.

Note that `filp_open`, `kernel_read` and `override_creds` have **no** safe Rust wrappers.
A substantial fraction of this work is raw `bindings::` + `unsafe`, not composition of safe
abstractions. Budget accordingly.

## Design

### Boundary

`/dev/xring`, a misc device. `open()` creates a per-fd ring context.

- **Control plane — `ENTER` ioctl.** Carries submissions in and completions out via
  `UserSlice`. `copy_from_user` gives the kernel a private snapshot by construction, which
  eliminates every shared-memory ordering hazard and every "lying head/tail" attack.
- **Data plane — `mmap`.** A buffer arena of N fixed-size slots, backed by order-0 `Page`s
  the *kernel* allocates and holds an owning reference to, inserted with `vm_insert_page`.

Since `ENTER` is the only entry point, a shared SQ would save zero syscalls. A shared CQ
would save syscalls on reaping — a real but deferrable optimization (T22).

### The central invariant

> **The kernel never dereferences a userspace address.** Every byte it touches lives in a
> `Page` it allocated and owns. No action available to userspace — `drop`, `mem::forget`,
> `munmap`, `close`, `_exit`, `kill -9` — can invalidate the kernel's target memory.

An SQE names a buffer by **slot index**, never a pointer; the kernel masks and bounds-checks
the index. Use-after-free is therefore not prevented by bookkeeping — it is unrepresentable.

Userspace ownership is then pure resource hygiene, and lives in a slab:

- The reactor owns a slab of `OpState` keyed by `user_data = (index: u32, generation: u32)`.
- `OpState` **owns** the `BufSlot`. The `Future` owns only the `user_data` cookie.
- `Future::drop` transitions `Live { waker } -> Abandoned` and submits `CANCEL`. It frees
  nothing.
- The completion handler removes the `OpState`; the `BufSlot` inside drops normally and
  returns to the pool.

The generation counter is load-bearing: without it a recycled slab index lets a late
completion from a cancelled op wake the wrong future.

`mem::forget` on a `BufSlot` leaks one slot of N — safe, since the index is never reused.
Dropping the whole ring mid-flight leaks the fd and arena until the last work item
finishes — also safe, because in-flight `OpWork`s hold their own `Arc<RingCtx>`.

### ABI invariants

These three are what make the ABI extensible and the ring unbrickable. Write them down and
test them.

- **C1 — every consumed SQE produces exactly one CQE.** Always: malformed SQEs, unknown
  opcodes, cancelled ops. io_uring violates this (`sq_dropped` silently drops SQEs); do not
  copy that. C1 reduces "completion never arrives" from a leak to a liveness question.
- **E1 — a bad SQE never fails the `ENTER` ioctl.** It yields a CQE with `res = -EINVAL`.
  Without E1, one bad SQE mid-batch leaves userspace unable to tell what was consumed.
  The ioctl return value reports *protocol* failures only, and on success is the count of
  SQEs consumed (never the completion count).
- **Reserved fields must be zero; unknown flag bits are rejected.** This is the only thing
  that permits adding fields later without breaking old binaries.

### Wire format

```rust
#[repr(C)] struct Sqe {              #[repr(C)] struct Cqe {
    opcode:    u8,                       user_data: u64,  // (slab_idx, generation)
    flags:     u8,                       res:       i64,  // >=0 result, <0 -errno
    _rsvd0:    u16,                      flags:     u32,  // CQE_F_MORE reserved
    len:       u32,                      _rsvd0:    u32,
    off:       u64,                      extra:     u64,
    user_data: u64,                  }   // 32 bytes
    slot:      u32,
    handle:    u32,                  // (index: u16, generation: u16)
}   // 32 bytes
```

`res: i64` because a 32-bit result is a permanent wart on any API that might grow large
reads or return offsets. 32 bytes so a CQE never straddles a cache line.

Opcodes: `NOP`, `DELAY_NS`, `OPEN`, `READ`, `CLOSE`, `CANCEL`, plus `CHECKSUM` as a
scaffolding op for T7.

### CQ overflow: admission control, not an overflow list

io_uring's overflow path `kmalloc`s under `GFP_ATOMIC`; allocation failure sets
`IO_CHECK_CQ_DROPPED_BIT` and the ring is **permanently bricked** (`-EBADR` forever).
Instead: require `cq_entries >= sq_entries`, track `outstanding = inflight + queued_cqes`
in kernel-private memory, and **reserve a CQ slot at SQE-consumption time**. If
`outstanding == cq_entries`, stop consuming and return a short submit count. Combined with
C1, the CQ can never overflow.

Price: **no multishot ops** without revisiting this. That is an explicit ABI constraint;
`CQE_F_MORE` reserves the bit for a future that solves it properly.

### Security rules

- **`OPEN` runs inline in `ENTER`**, in the submitting task's context, so `current_cred()`,
  `current->fs` and `current->nsproxy` are correct and LSM/audit see the right task.
  `filp_open` sleeps, which is fine in ioctl context.
- **Handles resolve to `ARef<File>` at submit time**, in ioctl context — a TOCTOU fix and a
  lifetime fix at once. A deferred op carries a fully-owned kernel snapshot (`Sqe` by value,
  `ARef<File>`, `CString`) with zero pointers into the arena.
- **Paths: copy out, then terminate, then scan.** Clamp `len <= min(slot_size, PATH_MAX)`
  with checked arithmetic (user-controlled `u32`/`u64` — overflow is trivially reachable),
  copy exactly `len` bytes into a `KVec<u8>`, reject an embedded NUL *in the kernel copy*,
  append our own NUL, build the `CString`. Never `strlen` in place; never hand a pointer
  into a mapped page to a VFS function.
- **Kernel-enforced slot exclusivity.** `index < N` is not enough — nothing stops userspace
  putting the same index in two concurrent SQEs, which races two `write_raw`s on one page.
  `RingCtx` holds a private `slot_busy` bitmap under the ring spinlock; a second op on a
  busy slot gets `-EBUSY` immediately. Userspace's pool becomes advisory, which is the
  right place for it.
- **Kernel writes to the arena are write-only; kernel reads are single-snapshot.** Never
  read back what we wrote.
- **Serialize submitters** with a `Mutex<Submitter>` across SQ consumption (io_uring's
  `uring_lock`). Required even though the queue is nominally single-producer, because the
  consumer must be single-threaded too.
- Device node ships `0600 root:root`; the permission model is explicitly unfinished.

### Lifetimes

```
Arc<RingCtx>                        (= MiscDevice::Ptr)
 ├─ arena:   KVec<Page>
 ├─ f_cred:  ARef<Credential>        captured at open()
 ├─ state:   SpinLock<RingState>     { cached_sq_head, cqes, slot_busy, inflight, dying }
 ├─ handles: Mutex<KVec<Option<..>>> Mutex not SpinLock — fput() sleeps
 └─ cq_wait: CondVar

KBox<OpWork>  ├─ work: Work<OpWork>  ├─ ring: Arc<RingCtx>  ├─ sqe: Sqe  └─ file: ARef<File>
```

`release()` takes the `Arc` by value; in-flight `OpWork`s hold their own, so the last one
out frees `RingCtx`. `release()` sets `dying`, notifies the CondVar, and drains the handle
table under the `Mutex`. It must **not** call `zap_vma_range` (no `->fault` handler through
`miscdevice`, so zapped PTEs mean SIGBUS) and must **not** touch user memory (`release` can
run from `____fput` on a kworker).

`ENTER` must use `CondVar::wait_interruptible_timeout`. Plain `wait()` makes the process
unkillable in D-state with an unloadable module. On signal return `-EINTR` — but still
report SQEs consumed, so userspace knows not to resubmit.

## C++20 userspace binding

The kernel side is **unchanged** — same device, same ioctls, same wire format, not one line
of new kernel code. That is the point: a second userspace implementation in an unrelated
language is the strongest available evidence that this is a language-neutral ABI rather
than a Rust idiom with a device node bolted on.

### C++20 coroutines fit this ABI better than Rust futures do

Rust futures are *poll*-based: the executor asks "are you ready?" and must re-poll after
every wakeup. That is a real impedance mismatch against a completion-based ring, and it is
why `tokio-uring` exists as a separate runtime at all.

C++20 coroutines are *continuation*-based. `await_suspend(h)` hands the reactor a
`std::coroutine_handle`; the reactor calls `h.resume()` when the CQE lands. No polling, no
`Waker` indirection, no spurious-wakeup case. Submission → completion → resume maps
one-to-one onto suspend → CQE → resume, so the awaiter is thinner than the Rust `Future`:

```
op_awaiter {
    await_ready()    -> false        // always suspends; the op is already submitted
    await_suspend(h) -> void         // store h in the slab entry; that is all
    await_resume()   -> result<T>    // read cqe.res back out of the slab entry
}
```

### Where C++ is weaker, and what actually carries the safety

Rust's move semantics make "the buffer is moved into the operation" a compile-time fact.
C++ cannot enforce that. `BufSlot` can be made move-only (deleted copy operations), but
use-after-move is UB the compiler will not catch.

This does **not** weaken the kernel's safety property, and saying so precisely is the whole
argument for the design:

> The central invariant — the kernel never dereferences a userspace address — is a property
> of the *ABI*, not of the language binding. A use-after-move bug in C++ can corrupt the
> program's own view of which slot holds what. It cannot produce a kernel use-after-free,
> cannot make the kernel touch freed memory, and cannot escape the process. Worst case, a
> program reads its own stale bytes.

That is a stronger guarantee than any pointer-passing ABI can offer a C++ client.

### The C++-specific hazard: dangling coroutine frames

The awaiter object lives *inside the coroutine frame*. If the frame is destroyed while an
op is in flight — an abandoned `task`, an exception unwinding a caller, an executor shut
down early — the reactor holds a `coroutine_handle` into freed memory, and resuming it is
UB. This is the exact analogue of "drop a Rust future mid-flight" and gets the same
treatment:

- **`~op_awaiter()` is the C++ spelling of `Future::drop`.** It marks the slab entry
  `Abandoned`, **moves the `BufSlot` out of the awaiter into the slab entry** (which
  outlives the frame), and submits `CANCEL`.
- The reactor never resumes an `Abandoned` entry — it discards it and recycles the slot
  when the CQE lands.
- The `(index, generation)` key does the same job as in Rust: a late completion for a
  cancelled op cannot resume a reused slab entry.

### Other C++ decisions to pin down

- **Symmetric transfer is mandatory.** `final_suspend()` must return an awaiter whose
  `await_suspend` *returns* the continuation handle rather than calling `.resume()` on it.
  Without it, a chain of N awaits consumes N stack frames. T19's done test is exactly this.
- **`await_suspend` must not touch `this` after publishing the handle.** Once the handle is
  visible to the reactor, the coroutine may already have been resumed and its frame
  destroyed. Harmless under the single-threaded executor, fatal the moment a waiter thread
  appears (T23) — so write the rule down now, not then.
- **No `std::expected` in C++20** (that is C++23). Define `xr::result<T>` holding either a
  value or an `errno`. Errors travel as values: an exception cannot cross the ABI boundary,
  and `unhandled_exception()` in a detached task has nowhere to send one. Define it as
  `std::terminate()` and document the runtime as exception-free by construction.
- **`task<T>` is lazy and move-only.** `initial_suspend()` returns `suspend_always`, so
  nothing runs until the task is awaited or handed to `sync_wait`. This makes ownership
  explicit and forecloses the classic detached-coroutine leak where `run()` returns with
  frames still suspended.
- **Coroutine frames heap-allocate.** HALO elision is real but unreliable across compilers;
  do not design around it. If allocation shows up in a profile, a pooled `operator new` on
  the promise type is the fix.
- **Toolchain**: GCC ≥ 11 or Clang ≥ 14, `-std=c++20`. Build with
  `-fsanitize=address,undefined` from day one — ASan catches the resumed-dangling-handle
  bug immediately, and that is the bug this binding is most likely to have.

## Known gaps, accepted for the PoC

- **`rmmod` hole.** An open fd pins the module; a queued work item does not. With no
  `cancel_work_sync`/`flush_workqueue` in Rust, close-then-`rmmod` with work pending is a
  UAF on module text. Fix with a module-scope live-work counter + `CondVar` in
  `Drop` (~25 lines, T6), or document "do not `rmmod` with ops in flight".
- **Cancellation is best-effort.** `DELAY_NS` via `enqueue_delayed` is genuinely
  cancellable. `OPEN`/`READ` already executing are not — no equivalent of io_uring setting
  `TIF_NOTIFY_SIGNAL` on a blocked worker. `CANCEL` returns `-EALREADY`. A per-op
  `cancel_requested: AtomicBool` checked at the top of `run()` covers not-yet-started work.
- **No `poll`, so no epoll/tokio integration.** A self-contained runtime does not need it —
  `park()` *is* `ENTER(min_complete=1)`, exactly as a pure-io_uring runtime works. If tokio
  integration is later required, the cheapest fix is a waiter thread that loops on `ENTER`
  and writes an eventfd (pure userspace, no kernel changes).
- **`timeout_ns` rounds to jiffy granularity** (`wait_interruptible_timeout` takes jiffies).
  Documented, not hidden. Clock is MONOTONIC, timeout relative.
- **Arena memory is unaccounted.** Unreclaimable, unswappable, not charged to any memcg,
  not counted against `RLIMIT_MEMLOCK`. Cap the arena size in `SETUP`.
- Deadlock foot-gun to close: `ENTER(to_submit=0, min_complete=1)` with nothing in flight
  must return immediately, not sleep. io_uring has this bug.

## Files

Created in dependency order:

- `kernel/xring_abi.rs` — `#[repr(C)]` SQE/CQE/params/ioctl definitions. Mirrored
  byte-for-byte by `user/xring-sys/src/abi.rs`. The most consequential file in the project.
- `kernel/xring.rs` — `MiscDevice` impl, the `Arc<RingCtx>` graph, admission control.
- `kernel/xring_ops.rs` — opcode dispatch, SQE validation, `OpWork`, and the raw
  `bindings::` calls for `filp_open`/`kernel_read`.
- `kernel/xring_arena.rs` — `KVec<Page>`, the `mmap` validation matrix, the
  `vm_insert_page` loop, slot busy tracking.
- `user/xring/src/lib.rs` — op slab, `OpState` owning `BufSlot`, `Future` impls, executor.
- `user/cpp/include/xring_abi.h` — the C mirror of `xring_abi.rs`, kept byte-identical by
  the T16 conformance test.
- `user/cpp/include/xring.hpp` — `Ring`, `BufPool`, move-only `BufSlot`, `result<T>`.
- `user/cpp/include/xring/task.hpp` — `task<T>` promise type, symmetric transfer,
  `sync_wait`.
- `user/cpp/include/xring/awaiter.hpp` — op slab, `op_awaiter`, the abandonment path.
- `user/cpp/examples/read_file.cpp` — the C++20 demo.

## Tasks

Ordering rationale: **control plane before memory plane.** The natural instinct is to mmap
first because that's the boundary, but that front-loads all the exploratory MM risk and
leaves nothing demoable if `vm_insert_page` turns into a swamp. Inverted, we have a fully
working async protocol at T6 — and the ABI, the part we would most regret getting wrong,
is settled while it is still cheap to change.

**[M]** mechanical · **[R]** risky/exploratory

### Phase 0 — environment

| # | Task | Done test |
|---|---|---|
| T0 **[R]** ✅ | Build and boot a Rust-enabled kernel on the Linux box (≥6.16, `make LLVM=1`, `CONFIG_RUST=y`, plus KASAN + `PROVE_LOCKING` + `DEBUG_KMEMLEAK`). Keep the full build tree — distro `linux-headers` omit `rust/*.rmeta` and cannot build OOT Rust modules. rustc floor 1.85.0, bindgen 0.71.1. Use an expendable VM: module bugs will panic the kernel. | **Done.** `modprobe rust_minimal` loads, `lsmod` lists it, `rmmod` unloads it, taint 0, under a 7.1.12 kernel with KASAN generic+inline, lockdep and kmemleak all confirmed live. See `CLAUDE.md` for the tree location and commands. |
| T1 **[R]** ✅ | Out-of-tree module skeleton: `Kbuild`, `make -C <tree> M=$PWD LLVM=1`. | **Done.** `kernel/xring.rs` + `kernel/Kbuild` build out-of-tree; `insmod`/`rmmod` cycle prints `init` then `exit`, taint is exactly 4096 (`TAINT_OOT_MODULE`) and nothing else, no oops or KASAN report. The multi-file crate layout below is verified: submodules reached by `mod` from `xring.rs`, correctly rebuilt when only a submodule changes. No need for the in-tree fallback; T0+T1 took about an hour. |
| T2 **[M]** ✅ | `MiscDevice` with `open`/`release` only; `Arc<RingCtx>` as `Ptr`. | **Done.** `/dev/xring` registers at 0600 root:root; 10,000 open/close iterations, zero failures, kmemleak clean before and after unload, taint 4096, no KASAN report. The scan is only meaningful after sleeping past kmemleak's 5 s `MSECS_MIN_AGE`; verified by a throwaway build leaking one `Arc` per open, which the test then reported (9,685 objects) and which an unslept scan had missed entirely. |

### Phase 1 — control plane, zero shared memory

| # | Task | Done test |
|---|---|---|
| T3 **[M]** ✅ | `SETUP` + `GET_PARAMS` ioctls via `UserSlice`. `_IOWR` encoding, magic + ABI version. `SETUP` callable exactly once, before `mmap`, with kernel-side caps. | **Done.** 24 assertions in `test/t3_setup.c`, all passing: params round-trip, `GET_PARAMS` legal before `SETUP`, magic/version mismatch → `-EPROTO`, unknown flags and non-zero reserved → `-EINVAL`, over-cap rejected not clamped, rejected attempts do not consume the one-shot, second `SETUP` → `-EBUSY`. Caps are reported in the struct rather than fixed as header constants, so raising one is not an ABI change. Verified by deleting three kernel checks and confirming exactly the three matching tests failed. |
| T4 **[M]** ✅ | `ENTER`: SQEs in, CQEs out via `UserSlice`. `NOP` only. Enforce reserved-zero and unknown-flag rejection. Unknown opcode → `-EINVAL` CQE (E1). | **Done.** 25 assertions in `test/t4_enter.c`. 8 NOPs in, 8 CQEs out, `user_data` matching in order. Unknown opcode, unknown SQE flag, non-zero `rsvd0` and non-zero unused field each yield a `-EINVAL` CQE with the ioctl still succeeding (E1); a mixed batch completes every entry (C1). Short `cq_space` leaves the remainder queued for the next `ENTER`. `Sqe`, `Cqe` (32 bytes) and `XringEnter` (64) are asserted field by field on both sides. Unused fields must be zero per opcode, so they stay available later. Admission control reserves a CQ slot before consuming an SQE, so submitting past `cq_entries` stops at a short count. Verified by breaking C1 and the capacity check and confirming exactly the 8 matching assertions failed. |
| T5 **[R]** | `CondVar` blocking wait; `DELAY_NS` via `enqueue_delayed`; `wait_interruptible_timeout`; admission control. | 4 × `DELAY_NS(50ms)` with `min_complete=4` returns in ~50 ms, not 200 (proves concurrency). 10 ms timeout against a 1 s delay returns 0 completions. Ctrl-C during `ENTER` → `-EINTR`, process killable. Submitting `cq_entries+1` ops → short submit count. |
| T6 **[R]** | Teardown torture; module live-work counter. | `close(fd)` with 4 × `DELAY_NS(5s)` in flight → no oops, kmemleak clean after they elapse. `kill -9` a task blocked in `ENTER` → clean. `rmmod` right after close with work pending → works, or is documented as forbidden. All under KASAN + lockdep. |

**T6 is the first demoable milestone: a working async syscall interface.** Everything after
is realism or optimization.

### Phase 2 — memory plane

| # | Task | Done test |
|---|---|---|
| T7 **[R]** | `mmap` the arena. Validate length, `vm_pgoff`, **`VM_SHARED`**, one-shot. `set_mixedmap` + `set_dontcopy` + `set_dontexpand`, then insert N order-0 pages. Add `CHECKSUM` (kernel `read_raw`s a slot, returns checksum in `res`) purely as scaffolding, so one thing is under debug at a time. | Pattern written to slot 3 checksums correctly. `MAP_PRIVATE` → `-EINVAL` (**silently gives COW and a maddening correctness bug otherwise — the single most likely day-loser**). Wrong length → `-EINVAL`. `fork()` → child's `/proc/self/maps` lacks the region. `munmap` mid-op → no oops. `close(fd)` without `munmap`, then `munmap` → no oops. *Budget a full day for the MAP_SHARED/fork/flags matrix alone.* |
| T8 **[M]** | Kernel-side slot busy tracking. | Two concurrent ops on one slot → second gets `-EBUSY`. Slot frees exactly when the CQE posts. |

### Phase 3 — real operations

| # | Task | Done test |
|---|---|---|
| T9 **[R]** | `OPEN`/`CLOSE` inline in `ENTER` (correct creds); generational handle table under `Mutex`; path copied out, NUL-scanned, `CString`. `CLOSE` bumps generation and drops `ARef<File>` outside the spinlock. | Open `/etc/hostname`, get handle, close. No fd leak in `/proc/<pid>/fd`. Double-close and stale generation → `-EBADF`. Embedded NUL → `-EINVAL`. **As an unprivileged user, opening `/etc/shadow` must fail `-EACCES` — write this creds regression test before writing the code.** |
| T10 **[M]** | `READ` into a slot, deferred to workqueue; `ARef<File>` resolved at submit. | Bytes match `cat`. `CLOSE` during an in-flight `READ` → no UAF under KASAN. Short read at EOF returns correct `res`. |
| T11 **[R]** | `CANCEL`: `res = 0` found/cancelled, `-ENOENT` unknown, `-EALREADY` already running. Target always gets its own CQE (C1); relative ordering unspecified. | Cancel a pending `DELAY_NS` → target `-ECANCELED`, canceller `0`. Cancel a running one → `-EALREADY`, target still completes. Both CQEs always arrive. |
| T12 **[R]** | **Hostile-userspace fuzz.** Random SQE bytes, random slot/handle/len/off, garbage reserved fields, 8 concurrent `ENTER` threads, concurrent `munmap`, random `kill -9`. | 10 minutes under KASAN + lockdep + kmemleak with zero kernel messages. **Do not skip — this is the only thing that validates the TOCTOU and validation rules.** |

### Phase 4 — Rust userspace

| # | Task | Done test |
|---|---|---|
| T13 **[M]** | `xring-sys` crate: `#[repr(C)]` ABI structs, ioctl wrappers, `Ring::{setup, enter, mmap}`, and a compile-time assertion that every struct's `size_of` matches the kernel's. | T4–T11's tests re-expressed as Rust integration tests, passing. |
| T14 **[R]** | Op slab keyed by `(index, generation)`; `Future` impls; single-threaded executor whose `park()` is `ENTER`. `BufSlot` owned by `OpState`, never by the future. | `async { let h = open("/etc/hostname").await?; let (n, buf) = read(h, buf).await?; print(&buf[..n]); close(h).await?; }` prints the file, concurrently with timers completing out of order. **This is the deliverable.** |
| T15 **[R]** | Drop-safety test. | Race a `read` future against a timer and drop it mid-flight. Assert a `CANCEL` is submitted, the slot is *not* in the free pool until the target's CQE lands, and a later op on that index does not get `-EBUSY`. 100k iterations under ASAN. |

### Phase 5 — C++20 userspace

No kernel changes. Depends only on T12 (a validated kernel), so it *can* run in parallel
with Phase 4 — but **do it after T14**. The Rust binding shakes the ABI out; writing the
second binding against a settled ABI is a test of language-neutrality, whereas writing both
at once just churns the wire format twice.

| # | Task | Done test |
|---|---|---|
| T16 **[M]** | `xring_abi.h` mirroring `xring_abi.rs`, with `static_assert` on every `sizeof` and `offsetof` and on every opcode value. Add an `abi_dump` binary to each side emitting a canonical text dump of the whole ABI surface. | `diff` of the two dumps is empty. Deliberately perturbing one field in either file makes the test fail — verify that, or the test proves nothing. |
| T17 **[M]** | `libxring` synchronous core: RAII `Ring` (open, `SETUP`, `mmap`, close), `BufPool`, move-only `BufSlot` (deleted copy ops), raw `submit()`/`reap()`, `result<T>`. No coroutines yet. | The entire T4–T11 test matrix re-expressed in C++ and passing — same assertions as T13, different language. Any divergence here is an ABI ambiguity worth fixing before coroutines hide it. |
| T18 **[R]** | Op slab + `op_awaiter`. `(index, generation)`-keyed slab of `op_state`; `await_ready`/`await_suspend`/`await_resume`; `~op_awaiter` marks `Abandoned`, moves the `BufSlot` into the slab entry, submits `CANCEL`. | Destroy a coroutine frame with an op in flight: under ASan, no resume of a freed handle; the slot is not recycled until the CQE lands; a later op on that index does not get `-EBUSY`. |
| T19 **[R]** | `task<T>` promise type: lazy `initial_suspend`, **symmetric transfer** on `final_suspend`, move-only, `unhandled_exception -> std::terminate`. `sync_wait(task<T>)`. | 100,000 nested `co_await`s complete with stack usage *measured* flat, not assumed — this is the symmetric-transfer regression test and it silently passes if you write it wrong and only try 10 levels. A task destroyed without being awaited leaks nothing under LSan. |
| T20 **[R]** | Executor: ready queue, `run()` whose park is `ENTER(min_complete=1, timeout)`, CQE → slab lookup → resume or discard. Close the same empty-ring deadlock foot-gun as the Rust side. | **The C++ demo**: `co_await open("/etc/hostname")`, `read`, print, `close`, concurrent with `delay` ops completing out of order — byte-identical output to the Rust demo. |
| T21 **[R]** | C++ drop-safety and cross-language interop. | Mirror of T15: race a read against a timer, destroy the frame mid-flight, 100k iterations under ASan + UBSan. Then run the Rust and C++ demos **concurrently** against the same module, each with its own ring, both producing correct output — the language-neutrality claim, actually tested rather than asserted. |

### Phase 6 — only if justified

- **T22 [R]** Shared mmap'd SQ/CQ with `Atomic<u32>` indices, behind a `features` bit,
  keeping the ioctl path as fallback. **Requires first solving finding #2** — obtaining a
  legitimate stable kernel VA into a `Page`, which may mean patching `rust/kernel/page.rs`.
  Re-examine whether it is worth it: with `ENTER`-per-submit the only win is syscall-free
  CQE reading. Done test: the whole T4–T12 suite passes in both modes, fuzz included, in
  **both** language bindings.
- **T23 [R]** eventfd registration + tokio `AsyncFd` bridge. Only if tokio integration
  becomes a goal, and try the pure-userspace waiter thread first. Note this is the point at
  which the "`await_suspend` must not touch `this`" rule stops being theoretical for C++.

## Verification

Every phase gate is a runnable test, listed above. Overall end-to-end:

1. `make -C <kernel-tree> M=$PWD LLVM=1 && insmod xring.ko`
2. `cargo test -p xring-sys` — ABI round-trip and per-opcode integration tests (T13).
3. `cargo run --example read_file` — the T14 demo: opens and reads a real file through
   coroutines while timers complete out of order.
4. `cargo run --example fuzz --release` for 10 minutes (T12), with `dmesg -w` in another
   terminal. Zero kernel messages is the pass condition.
5. `cmake -B build -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" && cmake --build build`
6. `diff <(./build/abi_dump) <(cargo run -q --bin abi_dump)` — empty (T16).
7. `ctest --test-dir build` — the C++ test matrix (T17), abandonment path (T18), symmetric
   transfer (T19) and drop-safety loop (T21), all under ASan + UBSan.
8. `./build/examples/read_file` — the C++20 demo (T20). Output must match step 3 byte for
   byte.
9. Run steps 3 and 8 **concurrently** (T21) — both must still be correct.
10. `echo scan > /sys/kernel/debug/kmemleak; cat /sys/kernel/debug/kmemleak` — empty.
11. Unprivileged-user creds test (T9) — must fail `-EACCES`.
12. `rmmod xring` cleanly at the end.

The dev kernel must have KASAN, `PROVE_LOCKING` and `DEBUG_KMEMLEAK` on from day one; they
pay for themselves in the first week.
