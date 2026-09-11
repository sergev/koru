# koru — design notes

The global picture: why this API is shaped the way it is, what the ABI
guarantees, and what building T0–T12 actually taught us. [Plan.md](Plan.md)
holds the remaining tasks and nothing else.

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
(`Work`, `DelayedWork`, `enqueue`, `enqueue_delayed` — but **no** cancel or
flush *wrapper*; the `bindings` have `cancel_delayed_work` and it is exported,
which is what T11 uses), `kernel::uaccess::UserSlice`.

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
never reused. Dropping the whole ring mid-flight is also safe: `release`
cancels everything still queued, and an op a worker has already picked up holds
its own `Arc<RingCtx>` and keeps the ring alive until it finishes.

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
vehicle. All seven are implemented.

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

`ENTER` must return rather than sleep whenever `min_complete` can never be
reached, which is precisely when nothing is in flight. That covers the io_uring
foot-gun of waiting on an empty ring, and also the case T11 found: asking for
more than is already queued while nothing is running.

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
 ├─ config:  Mutex<Option<RingConfig>>
 ├─ arena:   Mutex<Arena>            KVec<Page> plus the one-shot mmap flag
 ├─ state:   SpinLock<RingState>     { cqes, slot_busy, reserved, inflight }
 ├─ handles: Mutex<HandleTable>      Mutex not SpinLock — fput() sleeps
 └─ cq_wait: CondVar

KBox<OpWork>  ├─ work: Work<OpWork>  ├─ ring: Arc<RingCtx>
              ├─ sqe:  Sqe           └─ file: ARef<File>
```

`release()` takes the `Arc` by value; in-flight `OpWork`s hold their own, so the
last one out frees `RingCtx`. `release()` drains the handle table under the
`Mutex` and drops the files outside it. It must **not** call `zap_vma_range` (no
`->fault` handler through `miscdevice`, so zapped PTEs mean SIGBUS) and must
**not** touch user memory (`release` can run from `____fput` on a kworker).

There is no `dying` flag. Nothing has needed one: `ENTER` cannot be in flight
once `release` runs, and no deferred op touches the handle table.

## What T0–T12 established

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

### Handles

A handle is `(index: u16, generation: u16)`, index low. Generations start at 1
and skip 0 on wrap, so a valid handle is never 0 — which is what lets every
other opcode keep demanding that the `handle` field be zero when it does not
read it. `CLOSE` bumps the generation, so a stale handle and a double close both
land on `-EBADF`.

The generation is what actually protects reuse, and the perturbation test showed
which check does which job. Deleting the generation comparison left the
stale-handle *rejection* passing, because the entry's file was already `None` at
that point; what broke was the reuse case, where a retired handle closed the
file that had taken its index. The `None` check catches the easy half. Only the
generation catches the half that matters.

The table is fixed-size and allocated at `SETUP`, from a new `handle_count`
parameter. That parameter and its `max_handles` cap were carved out of
`KoruParams::reserved`, which shrank from `[u64; 4]` to `[u64; 3]` with the
struct staying 104 bytes. This is the first real exercise of the
reserved-must-be-zero rule, and it worked exactly as intended: an old caller
zeroes the word, and zero is the encoding for "give me the default". No version
bump, no size change, no change to any existing test.

`OPEN` carries its flags in the SQE's `handle` field, which it has no other use
for. They are koru's own bit values, not the host `O_*` constants, which differ
between architectures; the kernel whitelists and translates. There is no
`O_CREAT`, because no field can carry a creation mode.

`OPEN` claims the slot it reads its path from, then releases it the moment the
copy is done — before `filp_open`, which can block on disk. So the claim window
is short, but it is not decorative: without it a concurrent `CHECKSUM` or `READ`
can write the page mid-copy, which is precisely what `read_raw`'s
no-concurrent-access precondition forbids. Deleting the claim makes the test's
`-EBUSY` case fail.

`release()` drains the table explicitly rather than letting the `Arc` drop do
it. That is not belt-and-braces: an in-flight `OpWork` holds its own
`Arc<RingCtx>`, so with a two-second `DELAY_NS` queued, dropping the `Arc` frees
nothing and the open files survive for those two seconds. The done test queues
exactly that delay and watches the count fall at `close`, not later.

**kmemleak cannot see a leaked handle**, and this was confirmed by deliberately
leaking one: `mem::forget` on the `ARef` in `CLOSE`, run 2,000 times, produced a
completely clean kmemleak scan. The leaked `struct file` is still *referenced*,
by our own table, so it is not a leak in kmemleak's sense at all. The instrument
that does see it is field 1 of `/proc/sys/fs/file-nr`, the count of allocated
`struct file`. `/proc/<pid>/fd` sees nothing either way, because `filp_open`
installs no descriptor.

Testing the creds rule needs the privilege drop to happen *between* opening the
device and submitting, since the device node is `0600 root:root` and an
unprivileged process cannot open it at all. The test forks, and the child calls
`setresuid` to `nobody` — which clears the capability sets on the transition
away from uid 0 — before submitting on the fd the parent already opened.
`/etc/shadow` gives `-EACCES` and `/etc/hostname` still succeeds. The arena is
`VM_DONTCOPY`, so the child cannot inherit the mapping and has to `mmap` the
ring itself after the fork.

### Reads

`READ` names the file by handle, the destination by slot, and the source by an
explicit file offset in `off`. The offset lives in the SQE rather than in
`f_pos`, because two concurrent reads on one handle would otherwise race on a
shared position; `kernel_read` with a caller-owned `loff_t` never touches
`f_pos`. There is no destination offset within the slot — the SQE has no field
for one — so a read always lands at slot offset 0.

`res` is the count actually transferred. A short read at EOF is a result, not an
error, and a read at or past EOF is 0. The rest of the slot keeps whatever was
there before; that is the caller's own data, never the kernel's, so there is
nothing to scrub and `res` is what says which bytes are valid.

**A `Page` cannot be read into directly.** `Page`'s mapping helpers are private
and closure-scoped, and `page_address` has no binding, so there is no lasting
kernel address to hand `kernel_read`. Doing the `kmap_local_page` by hand would
work but would inhibit migration for the whole blocking read, and the highmem
documentation is explicit that these are short-term mappings. So the read
bounces through one page-sized buffer per op. Slots are whole pages, so a chunk
never straddles a page and the destination offset is just `pos % PAGE_SIZE`.

`vfs_read` is the wrong call and would not even link: its buffer is `__user`, it
runs `access_ok`, and it is not exported. The old `set_fs(KERNEL_DS)` escape was
removed tree-wide by 5.18. `kernel_read` exists precisely to replace it.

**The lockdep deadlock.** The first version held the arena `Mutex` across
`kernel_read`, and lockdep rejected it with a three-link cycle: `mmap` takes the
arena mutex under `mmap_lock`, a filesystem read takes `mmap_lock` under the
inode rwsem (to pin the user pages of a direct read), and `kernel_read` takes
that rwsem. So `arena → i_rwsem → mmap_lock → arena`. The fix is to hold the
arena lock only around each `write_raw`, never across the read. This generalises
the rule the ring spinlock already had: **the arena mutex is taken under
`mmap_lock`, so nothing may hold it across a call that can reach the VFS.**

**Two readability guards, and each earns its place.** `check_readable` rejects a
non-`S_IFREG` file and separately rejects a file whose `f_op` has `read` set or
`read_iter` unset. Deleting either one alone fails no test, because both catch
the directory case; deleting both lets the read reach `__kernel_read`, which
answers `-EINVAL` and logs `kernel read not supported for file`. They are kept
apart because they protect different things: only the `S_IFREG` check gives the
blocking-file-type property, since a FIFO uses `read_iter` and would sail
through the other one.

**Reads are restricted to regular files**, and `OPEN` to regular files and
directories. A blocking read inside a kworker cannot be interrupted — `CANCEL`
is best-effort and cannot touch work that has already started — so a read on a
FIFO, socket or tty would consume a system workqueue thread for good. This is a
deliberate limit; lifting it needs a non-blocking path that does not exist.

**A deferred op resolves every resource it needs at submit time** and then owns
it outright: `OpWork` carries the `Sqe` by value, an `Arc<RingCtx>`, and an
`ARef<File>`. It holds no index into anything that could be reindexed and no
pointer into the arena. That is what makes a `CLOSE` racing an in-flight `READ`
uninteresting rather than fatal, and the perturbation proves it — capturing the
file's address at submit time without taking the reference gives an immediate
KASAN slab-use-after-free.

### Cancellation

`CANCEL` names its target by `user_data` in `off`. A duplicate `user_data`
cancels one of them, unspecified which; both bindings key their op slabs by a
unique `(index, generation)`, so a duplicate is a userspace bug.

Cancellation is **real, not advisory**, which contradicts the earlier reading of
the workqueue API. The Rust wrapper exposes no cancel or flush at all, but
`cancel_delayed_work` is generated in the bindings and is a plain
`EXPORT_SYMBOL`, so an out-of-tree module can call it. Its return value is
exactly the fact needed: `true` means this call took the pending token from an
armed timer or a worklist entry, so the work function will never run.

**The refcount rule, which is the whole risk.** `enqueue_delayed` returning `Ok`
leaks one `Arc` strong reference, and the `run()` trampoline is the only thing
that ever reclaims it. A successful cancel therefore orphans that reference and
the canceller must drop exactly one, via `Arc::from_raw` on the pointer
`Arc::as_ptr` yields. A `false` return means nothing was pending and **nothing
may be dropped**. Both directions were tested by breaking them: omitting the
reclaim leaks the op, which leaks its `ARef<File>` and makes `rmmod` fail
because `PinnedDrop` never runs its `module_put`; reclaiming unconditionally is
an immediate KASAN slab-use-after-free.

In-flight deferred ops live in `Mutex<KVec<Arc<OpWork>>>` on `RingCtx`. That is
a reference cycle, since `OpWork` holds an `Arc<RingCtx>`, and it is broken by
removing the entry on every completion path. `run()` removes its entry **after**
calling `complete()`, not before, which is what makes a cancel arriving during
execution report `-EALREADY` rather than `-ENOENT`.

**`-EALREADY` is reachable by construction but was never observed** in 9,000
attempts across three race loops, including one against a `CHECKSUM` over a
whole slot, the longest-running op there is. The window is the span between the
worker clearing the pending bit and `run()` unregistering; a cancel either
arrives while the op is still queued, or after it has fully completed. So the
unregister-after-complete ordering is reasoned, not tested. Treat it as
unverified until something exercises it.

### `ENTER` could still sleep forever

T11 found a liveness bug T5 thought it had closed. The wait loop returned early
only when the ring was completely idle, `len == 0 && inflight == 0`. But a
caller asking for more completions than are queued, with nothing in flight, is
equally unsatisfiable — and slept until its watchdog fired. A cancel makes this
easy to reach: it leaves one CQE queued and nothing running.

The condition is now just `inflight == 0`. Completions come only from deferred
ops, so once nothing is in flight the queued count can never grow and any
unreached `min_complete` is unreachable for ever. The general lesson is that
"nothing can arrive" is a statement about `inflight` alone; bringing `len` into
it turns a short count into a hang.

### What ten minutes of hostile userspace actually proves

Eight million SQEs consumed across two million `ENTER` calls, from eight threads
on one ring, with faulting submission and completion buffers, malformed ioctl
numbers, rival `SETUP`s and `mmap`s, `munmap` under in-flight work, `SIGUSR1`
storms to force `-EINTR`, and a stream of forked children taking `SIGKILL` at
random points. Zero kernel
messages, no kmemleak objects, a clean `rmmod`, and taint exactly
`TAINT_OOT_MODULE`.

The load-bearing assertion is C1: consumed SQEs equalled reaped CQEs exactly,
every time. That is the invariant the whole ABI rests on, and the one a
userspace binding cannot recover from if it is ever false.

Every opcode both succeeded and failed, which is the part worth checking before
believing any of it. The counts are wildly uneven: `NOP`, `DELAY_NS` and
`CHECKSUM` succeed hundreds of thousands of times, while `OPEN` manages about a
thousand and `READ` a couple of hundred. The handle table saturates and stays
saturated, so most `OPEN`s answer `-EMFILE` and most `READ`s never get a live
handle. Those paths are genuinely exercised, but by hundreds of operations
rather than millions, and a shorter run gives a better ratio than a longer one.

**A legitimate long delay is a self-inflicted denial of service**, and the
fuzzer found it by hanging. `DELAY_NS` at the cap is accepted and then holds a
CQ reservation for an hour; a few thousand of those fill the queue and every
later submit returns a short count, while any `ENTER` waiting with no timeout
blocks until one fires. The cap bounds it and `release` clears it at `close`,
but inside a live ring it is still reachable. So the random phase now probes
only the *rejected* side of the cap, and the accepted side is covered once in
the deterministic phase, where it is cancelled immediately. A fuzzer must not
generate workloads that brick the thing it is fuzzing.

### What the fuzz changed before it ran

Surveying the module against a hostile-input checklist turned up four things
that were not crashes, and all four were fixed rather than recorded.

**`release` now cancels whatever is still queued.** A queued `DELAY_NS` used to
survive `close(fd)`, holding a module reference and the ring with it, and since
`off` was an unbounded nanosecond count, roughly 49 days was reachable. Any
process able to open the device could block `rmmod` indefinitely. `release`
walks the pending registry under its lock, cancels, and adopts the orphaned
reference for each cancel that succeeds; the registry vector is then swapped out
and dropped outside the lock, which is where the `fput`s and `module_put`s
happen. No completions are posted: the ring is being destroyed and no ioctl can
be in progress, because the VFS holds a reference for the duration of one.

**This inverted a T6 assertion.** `t6-donetest.sh` asserted that `rmmod` was
refused with work pending after the fd closed. It now asserts the opposite, and
checks the timing: `rmmod` must succeed within a second or two, because taking
five seconds would mean the delays were waited out rather than cancelled. That
is a stronger property than the one it replaced. `rmmod` with an fd still open
stays refused.

The other three: `cq_space` is now bounded by `cq_entries`, where it was
unbounded and `0xffffffff` sized a 137 GB `UserSlice`; `DELAY_NS` is capped by a
new `max_delay_ns`, carved out of `reserved[2]` the way T9 carved
`handle_count`; and the ioctl **direction** bits are checked, where dispatch
previously used only type, number and size, so a read-only encoding of `SETUP`
was accepted.

### A fuzzer's oracle is the hard part, not its randomness

The first version checked that every `res` was in its opcode's allowed set, that
every CQE had zero `flags`, `rsvd0` and `extra`, and that consumed SQEs equalled
reaped CQEs. Deleting the `rsvd0` rejection from `dispatch` did not fail it: the
op simply executed and succeeded, and success is in the allowed set. **A
value-range oracle cannot see a check that was removed.** The extensibility
rules are exactly the ones randomness is worst at, so they are now asserted
directly, in a deterministic phase that runs before the random one: for every
opcode, a non-zero `rsvd0` and an unknown flag bit must both be `-EINVAL`.

**Coverage counters mattered more than the assertions.** Printing per-opcode
completed-versus-succeeded totals showed `OPEN` succeeding 26 times in 7,192 and
`READ` 12 times in 6,187 — the fuzz was passing while barely touching the paths
it existed to test. Four separate causes, each invisible without the counters:
the path length did not match the path written, so half of all `OPEN`s tripped
the embedded-NUL rejection; eight threads shared one arena slot, so one thread's
`put_path` zeroed another's path mid-flight; the handle table filled and stayed
full because nothing tracked live handles to close; and the chaos thread was
reaping completions into its own buffer, swallowing `OPEN` handles that were
then orphaned. A fuzzer that reports no coverage is indistinguishable from one
that is not running.

### The slot bitmap is a live audit item

`RingState::slot_try_acquire` indexes `slot_busy` with no bounds check of its
own. That is sound today because every caller validates `slot < slot_count`
first, but it is one forgotten check away from being reachable. Deleting
`READ`'s slot check and issuing a single read with a large slot gives:

```
rust_kernel: panicked at koru.rs:184:26:
index out of bounds: the len is 1 but the index is 16384
kernel BUG at rust/helpers/bug.c:7!
Oops: invalid opcode: 0000 [#1] SMP KASAN NOPTI
```

Single-threaded, the panic kills the calling task with `SIGSEGV`. Under the
fuzzer's eight threads it wedges the guest outright, with no console output,
because the panic is taken while holding the ring spinlock. So the fuzzer does
catch it — but only as a dead machine, not a diagnosis. **Any new opcode that
calls `slot_try_acquire` must validate the slot first**, and making the bitmap
bounds-check itself would turn that class of mistake from an Oops into a failed
operation.

Note also that the bitmap is a whole number of 64-bit words, so with a small
`slot_count` there are spare bits: `slot_count + 1` stays in bounds and hides a
missing check. Probing past the word boundary is what makes the hole visible,
which is why the fuzzer generates slots past 64 and near `u32::MAX`.

### The done tests were only advisory

The dmesg check at the end of every done-test script printed splats without
failing the run. T10's lockdep deadlock therefore reported `PASS` on its first
green run, with the cycle sitting in the output above the pass line. Every
script now captures that grep into a variable and gates the pass on it being
empty, and the pattern list includes `kernel read not supported for file`,
which is a `pr_warn_ratelimited` rather than a `WARN_ON` and so matched nothing
before.

The lesson generalises past this bug: an assertion suite that passes proves the
code does what the test expects, and says nothing about what the kernel thinks
of it. On a debug kernel the kernel's own opinion has to be a hard gate.

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
- **Cancellation is real up to the point of execution, and nothing beyond.** A
  queued or timer-armed op is genuinely dequeued, by `CANCEL` or by `release`.
  An op a worker has already picked up cannot be stopped — there is no
  equivalent of io_uring setting `TIF_NOTIFY_SIGNAL` on a blocked worker — and
  `CANCEL` returns `-EALREADY`.
  The `cancel_requested: AtomicBool` this note used to propose is gone:
  `cancel_delayed_work` already handles every case the flag would have, and two
  mechanisms for one job is worse than one.
- **No `poll`, so no epoll/tokio integration.** A self-contained runtime does
  not need it — `park()` *is* `ENTER(min_complete=1)`, exactly as a
  pure-io_uring runtime works. If tokio integration is later required, the
  cheapest fix is a waiter thread that loops on `ENTER` and writes an eventfd
  (pure userspace, no kernel changes).
- **`timeout_ns` rounds to jiffy granularity** (`wait_interruptible_timeout`
  takes jiffies). Documented, not hidden. Clock is MONOTONIC, timeout relative.
- **Arena memory is unaccounted.** Unreclaimable, unswappable, not charged to
  any memcg, not counted against `RLIMIT_MEMLOCK`. Capped in `SETUP`.
- **`CLOSE` does `fput`, not `filp_close`,** so `->flush` never runs. It has to
  be `fput`: `ARef<File>` is what will let a T10 `READ` outlive a `CLOSE`, and
  you cannot `filp_close` a file another reference still holds. Invisible for
  regular files, which have no `->flush`; visible on NFS and FUSE.
- **A blocking `OPEN` stalls the whole ring.** It runs inline, so it holds
  `submit_lock` for the rest of its batch and every other submitter waits behind
  a slow path lookup. That is the direct price of resolving in the submitting
  task's context, and there is no version of this that both runs inline and does
  not block. The file-type restriction does not close this: `filp_open` on a
  FIFO blocks *before* we ever see the file, so the check cannot run. Closing it
  properly means passing `O_NONBLOCK`, which changes what a later `READ` does.
- **The arena is per fd and unaccounted**, so N open fds is N × the arena cap of
  unreclaimable memory. The device node is `0600 root:root`, which is the only
  thing bounding it.
- **Handles are per-ring**, so two rings in one process cannot share an open
  file. Nothing needs it yet.

## Files

In dependency order. The kernel files marked *exists* are written; the rest
arrive with their tasks.

- `kernel/koru_abi.rs` — `#[repr(C)]` SQE/CQE/params/ioctl definitions.
  Mirrored byte-for-byte by `user/koru-sys/src/abi.rs`. The most consequential
  file in the project. *exists*
- `kernel/koru.rs` — `MiscDevice` impl, the `Arc<RingCtx>` graph, admission
  control. *exists*
- `kernel/koru_ops.rs` — opcode dispatch, SQE validation, `OpWork`, and the raw
  `bindings::` calls for `filp_open`/`kernel_read`. *exists*
- `kernel/koru_arena.rs` — `KVec<Page>`, the `mmap` validation matrix, the
  `vm_insert_page` loop, slot busy tracking. The arena code is still in
  `koru.rs`; split it out when it grows enough to be worth the churn.
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
