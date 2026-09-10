# koru — design notes

The global picture: why this API is shaped the way it is, what the ABI
guarantees, and what building T0–T8 actually taught us. [Plan.md](Plan.md) holds
the remaining tasks and nothing else.

## Context

POSIX's syscall shape is synchronous: a trap is a rendezvous, control leaves and
returns at the same point. That is fundamentally at odds with coroutines, where
the caller must yield to its executor between "call" and "return". This project
designs a fresh kernel API built for async/await from the start — no POSIX
compatibility, no `errno`, no fds — and proves it with a working userspace
program driven entirely through the new interface.

The kernel side is an out-of-tree Rust module for dispatch. Two independent
userspace bindings sit on top of the same unchanged ABI: a Rust crate with
`Future` impls, and a C++20 header library with `co_await`-able awaiters. Each
has its own executor.

The defining constraint, chosen deliberately: **a coroutine can never own
kernel-visible memory.** Rust futures can be dropped at any await point
(`select!`, timeouts, cancelled tasks), and `Drop` cannot be async — so any
design where an in-flight operation holds a userspace pointer has an unavoidable
use-after-free. We remove the possibility rather than manage it.

Outcome: `/dev/koru`, a Rust module implementing it, and two demos — one Rust,
one C++20 — that each open and read a real file through coroutines while timers
complete out of order.

## Research findings that shaped this design

Verified against mainline `rust/kernel/` and kernel docs. Four findings changed
the approach materially:

1. **`uring_cmd` has no Rust abstraction.** Only Sidong Yang's unmerged
   [RFC v3](https://lkml.org/lkml/2025/8/22/1054), with open soundness bugs. We
   build our own transport instead of layering on io_uring.
2. **A shared-memory SQ/CQ ring is not implementable with merged Rust APIs.**
   `Page::with_page_mapped` / `with_pointer_into_page` are private. The only
   public accessors — `read_raw`, `write_raw`, `fill_zero_raw` — are `memcpy`
   under a transient `kmap_local_page`, with a documented precondition of *no
   concurrent access*. A ring shared with untrusted userspace violates that by
   definition, and they give no way to place an `Atomic<u32>` over a shared word
   or to do an acquire-load. **Consequence: the control plane is an ioctl using
   `UserSlice` (`copy_from_user`), not shared memory.** Only the data-plane
   arena is mmap'd.
3. **Deferring `open()` to a workqueue is a root privilege escalation.** In a
   kworker, `current_cred()` is `&init_cred` and `current->fs` is the init root
   — `filp_open` would resolve and permission-check as root in the initial
   namespaces. io_uring handles this with `override_creds` plus io-wq workers
   that clone the ring creator's `mm`/`fs`/`nsproxy`; `kernel::cred::Credential`
   exposes no such wrapper. **Consequence: `OPEN` runs inline in the submitting
   task's context.**
4. **No async executor exists in kernel Rust.** No `Future`, no `kasync`;
   `kernel::workqueue` is callback-based. The coroutines live entirely in
   userspace; the kernel is a dispatch table plus a worker pool — the same split
   io_uring makes with `io-wq`. This is a permanent property of the design, not
   a temporary shortcut.

Merged and usable: `kernel::miscdevice` (6.13;
`open`/`release`/`ioctl`/`mmap`/`read_iter`, **no `poll`**), `kernel::mm::virt`
(6.16; `VmaNew::set_mixedmap`, `VmaMixedMap::vm_insert_page`),
`kernel::sync::{CondVar, SpinLock, Mutex, Arc, atomic}`, `kernel::workqueue`
(`Work`, `DelayedWork`, `enqueue`, `enqueue_delayed` — but **no**
`cancel_work_sync` or `flush_workqueue`), `kernel::uaccess::UserSlice`.

Note that `filp_open`, `kernel_read` and `override_creds` have **no** safe Rust
wrappers. A substantial fraction of this work is raw `bindings::` + `unsafe`,
not composition of safe abstractions. Budget accordingly.

## Design

### Boundary

`/dev/koru`, a misc device. `open()` creates a per-fd ring context.

- **Control plane — `ENTER` ioctl.** Carries submissions in and completions out
  via `UserSlice`. `copy_from_user` gives the kernel a private snapshot by
  construction, which eliminates every shared-memory ordering hazard and every
  "lying head/tail" attack.
- **Data plane — `mmap`.** A buffer arena of N fixed-size slots, backed by
  order-0 `Page`s the *kernel* allocates and holds an owning reference to,
  inserted with `vm_insert_page`.

Since `ENTER` is the only entry point, a shared SQ would save zero syscalls. A
shared CQ would save syscalls on reaping — a real but deferrable optimization
(T22).

### The central invariant

> **The kernel never dereferences a userspace address.** Every byte it touches
> lives in a `Page` it allocated and owns. No action available to userspace —
> `drop`, `mem::forget`, `munmap`, `close`, `_exit`, `kill -9` — can invalidate
> the kernel's target memory.

An SQE names a buffer by **slot index**, never a pointer; the kernel masks and
bounds-checks the index. Use-after-free is therefore not prevented by
bookkeeping — it is unrepresentable.

### Ownership in userspace

Userspace ownership is then pure resource hygiene, and lives in a slab:

- The reactor owns a slab of `OpState` keyed by
  `user_data = (index: u32, generation: u32)`.
- `OpState` **owns** the `BufSlot`. The `Future` owns only the `user_data`
  cookie.
- `Future::drop` transitions `Live { waker } -> Abandoned` and submits `CANCEL`.
  It frees nothing.
- The completion handler removes the `OpState`; the `BufSlot` inside drops
  normally and returns to the pool.

The generation counter is load-bearing: without it a recycled slab index lets a
late completion from a cancelled op wake the wrong future.

`mem::forget` on a `BufSlot` leaks one slot of N — safe, since the index is
never reused. Dropping the whole ring mid-flight leaks the fd and arena until
the last work item finishes — also safe, because in-flight `OpWork`s hold their
own `Arc<RingCtx>`.

### ABI invariants

These three are what make the ABI extensible and the ring unbrickable. Write
them down and test them.

- **C1 — every consumed SQE produces exactly one CQE.** Always: malformed SQEs,
  unknown opcodes, cancelled ops. io_uring violates this (`sq_dropped` silently
  drops SQEs); do not copy that. C1 reduces "completion never arrives" from a
  leak to a liveness question.
- **E1 — a bad SQE never fails the `ENTER` ioctl.** It yields a CQE with
  `res = -EINVAL`. Without E1, one bad SQE mid-batch leaves userspace unable to
  tell what was consumed. The ioctl return value reports *protocol* failures
  only, and on success is the count of SQEs consumed (never the completion
  count).
- **Reserved fields must be zero; unknown flag bits are rejected.** This is the
  only thing that permits adding fields later without breaking old binaries.

### Wire format

```rust
#[repr(C)] struct Sqe {         #[repr(C)] struct Cqe {
    opcode:    u8,                  user_data: u64,  // (slab_idx, generation)
    flags:     u8,                  res:       i64,  // >=0 result, <0 -errno
    _rsvd0:    u16,                 flags:     u32,  // CQE_F_MORE reserved
    len:       u32,                 _rsvd0:    u32,
    off:       u64,                 extra:     u64,
    user_data: u64,             }   // 32 bytes
    slot:      u32,
    handle:    u32,             // (index: u16, generation: u16)
}   // 32 bytes
```

`res: i64` because a 32-bit result is a permanent wart on any API that might
grow large reads or return offsets. 32 bytes so a CQE never straddles a cache
line.

Opcodes: `NOP`, `DELAY_NS`, `OPEN`, `READ`, `CLOSE`, `CANCEL`, plus `CHECKSUM`,
which began as scaffolding for the arena and stayed as the deferred-op test
vehicle. `NOP`, `DELAY_NS` and `CHECKSUM` are implemented.

The ioctl type byte is `'k'` and the magic word is `0x7572_6f6b`, spelling
"koru" little-endian. That type byte is listed as conflicting in the kernel's
`ioctl-number.rst`, since spidev claims 00-0F and kyro claims 00-05 over our
commands 00-02. Harmless in practice: the fd identifies the driver.

### Wait semantics

`min_complete` alone decides whether `ENTER` blocks. `timeout_ns` only caps the
wait, and 0 means no cap. `ENTER` returns the count of SQEs consumed on success
and `-EINTR` on a signal; either way `submitted` and `completed` are written
back, so an interrupted call still says what must not be resubmitted.

`ENTER` must use `CondVar::wait_interruptible_timeout`. Plain `wait()` makes the
process unkillable in D-state with an unloadable module.

`ENTER(to_submit = 0, min_complete = 1)` with nothing in flight must return
immediately rather than sleep. io_uring has this deadlock foot-gun; ours is
closed and has a test.

### CQ overflow: admission control, not an overflow list

io_uring's overflow path `kmalloc`s under `GFP_ATOMIC`; allocation failure sets
`IO_CHECK_CQ_DROPPED_BIT` and the ring is **permanently bricked** (`-EBADR`
forever). Instead: require `cq_entries >= sq_entries`, track
`outstanding = inflight + queued_cqes` in kernel-private memory, and **reserve a
CQ slot at SQE-consumption time**. If `outstanding == cq_entries`, stop
consuming and return a short submit count. Combined with C1, the CQ can never
overflow.

Price: **no multishot ops** without revisiting this. That is an explicit ABI
constraint; `CQE_F_MORE` reserves the bit for a future that solves it properly.

### Security rules

- **`OPEN` runs inline in `ENTER`**, in the submitting task's context, so
  `current_cred()`, `current->fs` and `current->nsproxy` are correct and
  LSM/audit see the right task. `filp_open` sleeps, which is fine in ioctl
  context.
- **Handles resolve to `ARef<File>` at submit time**, in ioctl context — a
  TOCTOU fix and a lifetime fix at once. A deferred op carries a fully-owned
  kernel snapshot (`Sqe` by value, `ARef<File>`, `CString`) with zero pointers
  into the arena.
- **Paths: copy out, then terminate, then scan.** Clamp
  `len <= min(slot_size, PATH_MAX)` with checked arithmetic (user-controlled
  `u32`/`u64` — overflow is trivially reachable), copy exactly `len` bytes into
  a `KVec<u8>`, reject an embedded NUL *in the kernel copy*, append our own NUL,
  build the `CString`. Never `strlen` in place; never hand a pointer into a
  mapped page to a VFS function.
- **Kernel-enforced slot exclusivity.** `index < N` is not enough — nothing
  stops userspace putting the same index in two concurrent SQEs, which races two
  `write_raw`s on one page. `RingCtx` holds a private `slot_busy` bitmap under
  the ring spinlock; a second op on a busy slot gets `-EBUSY` immediately.
  Userspace's pool becomes advisory, which is the right place for it.
- **Kernel writes to the arena are write-only; kernel reads are
  single-snapshot.** Never read back what we wrote.
- **Serialize submitters** with a `Mutex<Submitter>` across SQ consumption
  (io_uring's `uring_lock`). Required even though the queue is nominally
  single-producer, because the consumer must be single-threaded too.
- **`ENTER` must never touch a `UserSlice` while holding the ring `SpinLock`**,
  because `copy_*_user` can fault and therefore sleep. The submit and reap loops
  are structured around that. The submitter `Mutex` is likewise dropped around
  the `CondVar` wait, or one waiter blocks every submitter.
- Device node ships `0600 root:root`; the permission model is explicitly
  unfinished.

### Lifetimes

```
Arc<RingCtx>                        (= MiscDevice::Ptr)
 ├─ arena:   KVec<Page>
 ├─ f_cred:  ARef<Credential>        captured at open()
 ├─ state:   SpinLock<RingState>     { cached_sq_head, cqes, slot_busy,
 │                                     inflight, dying }
 ├─ handles: Mutex<KVec<Option<..>>> Mutex not SpinLock — fput() sleeps
 └─ cq_wait: CondVar

KBox<OpWork>  ├─ work: Work<OpWork>  ├─ ring: Arc<RingCtx>
              ├─ sqe:  Sqe           └─ file: ARef<File>
```

`release()` takes the `Arc` by value; in-flight `OpWork`s hold their own, so the
last one out frees `RingCtx`. `release()` sets `dying`, notifies the CondVar,
and drains the handle table under the `Mutex`. It must **not** call
`zap_vma_range` (no `->fault` handler through `miscdevice`, so zapped PTEs mean
SIGBUS) and must **not** touch user memory (`release` can run from `____fput` on
a kworker).

## What T0–T8 established

The tasks themselves are gone from [Plan.md](Plan.md); what they proved is here.

### Environment and module skeleton

The dev kernel is 7.1.12 with KASAN generic+inline, `PROVE_LOCKING` and
`DEBUG_KMEMLEAK` all confirmed live. The full build tree has to be kept: distro
`linux-headers` omit `rust/*.rmeta` and cannot build out-of-tree Rust modules at
all. Tree location and commands live in `CLAUDE.md`.

Expected taint with the module loaded is exactly 4096, `TAINT_OOT_MODULE`.
Module signing is off, so bit 13 must never appear; anything other than 4096 is
a finding.

The multi-file crate layout works: only `koru.rs` is named in `kernel/Kbuild`
and the other kernel `.rs` files are submodules reached by `mod`, correctly
rebuilt when a submodule alone changes. The in-tree fallback was never needed.

`/dev/koru` registers at 0600 root:root and survives 10,000 open/close
iterations with zero failures.

### kmemleak lies about young objects

**kmemleak reports nothing about an object younger than five seconds**
(`MSECS_MIN_AGE` in `mm/kmemleak.c`). Scanning right after a test loop reports a
clean result no matter how badly the code leaks. Every leak check must sleep
past that age first; the T2 script sleeps 8 seconds, then scans twice, because
the first pass after heavy allocation is not settled.

This was caught by deliberately leaking an `Arc` per open and finding the check
silent. The instrumented build then reported 9,685 objects. Treat any new leak
test as untrustworthy until it has been shown to fail on a real leak.

### Control plane

Caps are reported in the params struct rather than fixed as header constants, so
raising one is not an ABI change. `GET_PARAMS` is legal before `SETUP`. A
magic or ABI-version mismatch is `-EPROTO`, distinct from `-EINVAL` for bad
content and `-ENOTTY` for an unknown command. `SETUP` is one-shot, a second call
is `-EBUSY`, and a rejected attempt does not consume the one shot.

Two ABI changes came out of the blocking-wait work, both already folded into the
wire format above. The `ENTER` reserved word became a `submitted` out-field,
because one ioctl return value cannot carry both `-EINTR` and the consumed
count. And `min_complete` became the thing that gates the wait, with
`timeout_ns` only capping it.

Admission control reserves a CQ slot before consuming an SQE, so submitting past
`cq_entries` stops at a short count rather than overflowing. A short `cq_space`
leaves the remainder queued for the next `ENTER`. Unused fields must be zero per
opcode, so they stay available for later use.

Four `DELAY_NS(50ms)` ops with `min_complete = 4` return in 51 ms, not 200, so
the workqueue really does run them in parallel.

### The module pins itself

`kernel::miscdevice` builds its `file_operations` with `..zeroed()`, so
`fops.owner` is NULL and `fops_get` pins nothing. Neither an open fd nor a
queued work item pins the module on its own; both were use-after-frees on module
text. `open`/`release` and the deferred op path now take and drop explicit
references, so `rmmod` returns `-EBUSY` rather than freeing text still in use.

Every new path that outlives an `ENTER` needs the same treatment, and a missing
`module_put` wedges the module as permanently unloadable.

### Memory plane

`slot_size` must be a multiple of `PAGE_SIZE`, so every slot is a whole number
of pages and nothing straddles a page boundary. This tightened the `SETUP`
validation after the fact. Pages are allocated and zeroed at `SETUP`, not at
`mmap`, so `mmap` never allocates and `SETUP` cannot promise memory it has not
got.

`mmap` is one-shot and demands `MAP_SHARED`, `vm_pgoff == 0` and a length
exactly equal to `arena_size`. `MAP_PRIVATE` would silently give copy-on-write,
which is the failure worth guarding hardest.

**`VM_IO` is deliberately not set.** The `set_dontcopy` doc says `VM_DONTCOPY`
is only permanent with `VM_IO`, but `MADV_DOFORK` actually refuses on
`VM_SPECIAL`, which includes `VM_MIXEDMAP` and `VM_DONTEXPAND`. Both are already
set, so userspace cannot undo it, and there is a test asserting the `madvise`
fails.

A slot is claimed in ioctl context after validation and released in the same
critical section that posts the CQE, so it is free exactly when userspace can
see the completion. An op refused a slot gets `-EBUSY` as its completion and
must not release the slot the winner holds.

**`CHECKSUM` had to become deferred** rather than inline. An inline op holds its
slot only inside the submit loop, so a collision could never happen and the
exclusivity rule would be untestable. That also pre-exercises the shape `READ`
needs.

### How a done test earns trust

Assert the exact errno, never just that a call failed. The T3 dispatcher
returned `EPROTO` where it owed `ENOTTY`, and only an exact-errno assertion
noticed.

Every rejection test was then confirmed to have teeth by deleting the
corresponding kernel check and watching that one test, and only that one, fail.
Do this for each new check. A test that has never been shown to fail is not
evidence.

A test that can hang must arm an alarm and be line-buffered. `_exit` from the
handler drops a full stdout buffer, which turns a diagnosable hang into a silent
one.

## C++20 userspace binding

The kernel side is **unchanged** — same device, same ioctls, same wire format,
not one line of new kernel code. That is the point: a second userspace
implementation in an unrelated language is the strongest available evidence that
this is a language-neutral ABI rather than a Rust idiom with a device node
bolted on.

### C++20 coroutines fit this ABI better than Rust futures do

Rust futures are *poll*-based: the executor asks "are you ready?" and must
re-poll after every wakeup. That is a real impedance mismatch against a
completion-based ring, and it is why `tokio-uring` exists as a separate runtime
at all.

C++20 coroutines are *continuation*-based. `await_suspend(h)` hands the reactor
a `std::coroutine_handle`; the reactor calls `h.resume()` when the CQE lands. No
polling, no `Waker` indirection, no spurious-wakeup case. Submission →
completion → resume maps one-to-one onto suspend → CQE → resume, so the awaiter
is thinner than the Rust `Future`:

```
op_awaiter {
    await_ready()    -> false      // always suspends; op already submitted
    await_suspend(h) -> void       // store h in the slab entry; that is all
    await_resume()   -> result<T>  // read cqe.res out of the slab entry
}
```

### Where C++ is weaker, and what actually carries the safety

Rust's move semantics make "the buffer is moved into the operation" a
compile-time fact. C++ cannot enforce that. `BufSlot` can be made move-only
(deleted copy operations), but use-after-move is UB the compiler will not catch.

This does **not** weaken the kernel's safety property, and saying so precisely
is the whole argument for the design:

> The central invariant — the kernel never dereferences a userspace address — is
> a property of the *ABI*, not of the language binding. A use-after-move bug in
> C++ can corrupt the program's own view of which slot holds what. It cannot
> produce a kernel use-after-free, cannot make the kernel touch freed memory,
> and cannot escape the process. Worst case, a program reads its own stale
> bytes.

That is a stronger guarantee than any pointer-passing ABI can offer a C++
client.

### The C++-specific hazard: dangling coroutine frames

The awaiter object lives *inside the coroutine frame*. If the frame is destroyed
while an op is in flight — an abandoned `task`, an exception unwinding a caller,
an executor shut down early — the reactor holds a `coroutine_handle` into freed
memory, and resuming it is UB. This is the exact analogue of "drop a Rust future
mid-flight" and gets the same treatment:

- **`~op_awaiter()` is the C++ spelling of `Future::drop`.** It marks the slab
  entry `Abandoned`, **moves the `BufSlot` out of the awaiter into the slab
  entry** (which outlives the frame), and submits `CANCEL`.
- The reactor never resumes an `Abandoned` entry — it discards it and recycles
  the slot when the CQE lands.
- The `(index, generation)` key does the same job as in Rust: a late completion
  for a cancelled op cannot resume a reused slab entry.

### Other C++ decisions to pin down

- **Symmetric transfer is mandatory.** `final_suspend()` must return an awaiter
  whose `await_suspend` *returns* the continuation handle rather than calling
  `.resume()` on it. Without it, a chain of N awaits consumes N stack frames.
  T19's done test is exactly this.
- **`await_suspend` must not touch `this` after publishing the handle.** Once
  the handle is visible to the reactor, the coroutine may already have been
  resumed and its frame destroyed. Harmless under the single-threaded executor,
  fatal the moment a waiter thread appears (T23) — so write the rule down now,
  not then.
- **No `std::expected` in C++20** (that is C++23). Define `koru::result<T>`
  holding either a value or an `errno`. Errors travel as values: an exception
  cannot cross the ABI boundary, and `unhandled_exception()` in a detached task
  has nowhere to send one. Define it as `std::terminate()` and document the
  runtime as exception-free by construction.
- **`task<T>` is lazy and move-only.** `initial_suspend()` returns
  `suspend_always`, so nothing runs until the task is awaited or handed to
  `sync_wait`. This makes ownership explicit and forecloses the classic
  detached-coroutine leak where `run()` returns with frames still suspended.
- **Coroutine frames heap-allocate.** HALO elision is real but unreliable across
  compilers; do not design around it. If allocation shows up in a profile, a
  pooled `operator new` on the promise type is the fix.
- **Toolchain**: GCC ≥ 11 or Clang ≥ 14, `-std=c++20`. Build with
  `-fsanitize=address,undefined` from day one — ASan catches the
  resumed-dangling-handle bug immediately, and that is the bug this binding is
  most likely to have.

The two userspace bindings share no code. That is intentional: the second
binding exists to demonstrate the ABI is language-neutral, so resist factoring
common logic across them.

## Known gaps, accepted for the PoC

- **`rmmod` remnant.** Both `module_put` calls run from module text, so a
  *blocking* `delete_module` could proceed as one returns. Default `rmmod` is
  non-blocking and fails fast on a non-zero count. The real fix is upstream:
  `MiscDeviceOptions` has no way to set `fops.owner`.
- **Cancellation is best-effort.** `DELAY_NS` via `enqueue_delayed` is genuinely
  cancellable. `OPEN`/`READ` already executing are not — no equivalent of
  io_uring setting `TIF_NOTIFY_SIGNAL` on a blocked worker. `CANCEL` returns
  `-EALREADY`. A per-op `cancel_requested: AtomicBool` checked at the top of
  `run()` covers not-yet-started work.
- **No `poll`, so no epoll/tokio integration.** A self-contained runtime does
  not need it — `park()` *is* `ENTER(min_complete=1)`, exactly as a
  pure-io_uring runtime works. If tokio integration is later required, the
  cheapest fix is a waiter thread that loops on `ENTER` and writes an eventfd
  (pure userspace, no kernel changes).
- **`timeout_ns` rounds to jiffy granularity** (`wait_interruptible_timeout`
  takes jiffies). Documented, not hidden. Clock is MONOTONIC, timeout relative.
- **Arena memory is unaccounted.** Unreclaimable, unswappable, not charged to
  any memcg, not counted against `RLIMIT_MEMLOCK`. Capped in `SETUP`.

## Files

In dependency order. The kernel files marked *exists* are written; the rest
arrive with their tasks.

- `kernel/koru_abi.rs` — `#[repr(C)]` SQE/CQE/params/ioctl definitions.
  Mirrored byte-for-byte by `user/koru-sys/src/abi.rs`. The most consequential
  file in the project. *exists*
- `kernel/koru.rs` — `MiscDevice` impl, the `Arc<RingCtx>` graph, admission
  control. *exists*
- `kernel/koru_ops.rs` — opcode dispatch, SQE validation, `OpWork`, and the raw
  `bindings::` calls for `filp_open`/`kernel_read`.
- `kernel/koru_arena.rs` — `KVec<Page>`, the `mmap` validation matrix, the
  `vm_insert_page` loop, slot busy tracking.
- `user/koru/src/lib.rs` — op slab, `OpState` owning `BufSlot`, `Future` impls,
  executor.
- `user/cpp/include/koru_abi.h` — the C mirror of `koru_abi.rs`, kept
  byte-identical by the T16 conformance test.
- `user/cpp/include/koru.hpp` — `Ring`, `BufPool`, move-only `BufSlot`,
  `result<T>`.
- `user/cpp/include/koru/task.hpp` — `task<T>` promise type, symmetric transfer,
  `sync_wait`.
- `user/cpp/include/koru/awaiter.hpp` — op slab, `op_awaiter`, the abandonment
  path.
- `user/cpp/examples/read_file.cpp` — the C++20 demo.

`test/` holds interim C programs, one per task, plus a shared harness. They are
scaffolding: the real suites are the Rust one at T13 and the C++ one at T17. The
harness header's first section is a hand-written mirror of `kernel/koru_abi.rs`,
so a change there means a matching change in that one place, and its
`_Static_assert`s are what catch you forgetting. T16 deletes that section in
favour of the real `user/cpp/include/koru_abi.h`.

## Why the tasks are ordered this way

**Control plane before memory plane.** The natural instinct is to mmap first
because that is the boundary, but that front-loads all the exploratory MM risk
and leaves nothing demoable if `vm_insert_page` turns into a swamp. Inverted, we
had a fully working async protocol at T6 — and the ABI, the part we would most
regret getting wrong, was settled while it was still cheap to change.

T6 was the first demoable milestone: a working async syscall interface.
Everything after it is realism or optimization.
