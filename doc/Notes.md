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
(T50).

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

`CONFIG_USER_NS=y` joined the fragment at T23, which is the only config change
since T0. It is not debug instrumentation: with it off the id-translation
helpers a `STAT` needs are static inlines bindgen never emits, and every id maps
one to one so the namespace handling cannot be shown to be wrong. See "Stat, and
the first result that does not fit in a CQE".

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
`O_CREAT`, because on this opcode `handle` is already spent on the flags and no
other field can carry a creation mode. T26's `MKDIR`, which has `handle` free,
is what shows that is a fact about `OPEN` rather than about the SQE.

`OPEN` claims the slot it reads its path from, then releases it the moment the
copy is done — before `filp_open`, which can block on disk. So the claim window
is short, but it is not decorative: without it a concurrent `CHECKSUM` or `READ`
can write the page mid-copy, which is precisely what `read_raw`'s
no-concurrent-access precondition forbids. Deleting the claim makes the test's
`-EBUSY` case fail.

`release()` drains the table explicitly rather than letting the `Arc` drop do
it. That is not belt-and-braces: an in-flight `OpWork` holds its own
`Arc<RingCtx>`, so with a two-second `DELAY_NS` queued, dropping the `Arc` frees
nothing and the open files survive for those two seconds. The check queues
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
file that is neither regular nor opened non-blocking, and separately rejects a
file whose `f_op` has `read` set or `read_iter` unset. They protect different
things: only the first gives the blocking-file-type property, since a FIFO uses
`read_iter` and would sail through the second. Before T18 neither could be
falsified alone, because both caught the directory case; since T18 a directory
opened `KORU_O_NONBLOCK` clears the first and is refused only by the second, so
each is now tested on its own.

**Reads are restricted to regular files, or to anything opened
`KORU_O_NONBLOCK`.** A blocking read inside a kworker cannot be interrupted —
`CANCEL` is best-effort and cannot touch work that has already started — so a
read on a FIFO, socket or tty would consume a system workqueue thread for good.
T18 lifted the limit the only way that is safe, by making the caller ask for a
file that cannot block. `OPEN` itself gates on nothing since T18; the rule lives
on `READ` and `WRITE`.

**A deferred op resolves every resource it needs at submit time** and then owns
it outright: `OpWork` carries the `Sqe` by value, an `Arc<RingCtx>`, and an
`ARef<File>`. It holds no index into anything that could be reindexed and no
pointer into the arena. That is what makes a `CLOSE` racing an in-flight `READ`
uninteresting rather than fatal, and the perturbation proves it — capturing the
file's address at submit time without taking the reference gives an immediate
KASAN slab-use-after-free.

### Writes

T17's `WRITE` is `READ` with the copy reversed: the same inline validate that
resolves the handle and claims the slot in ioctl context, the same deferral to
the workqueue, the same one page-sized bounce buffer, and the arena mutex held
only around the page copy and never across the VFS call. `kernel_write` rather
than `__kernel_write`, which skips `rw_verify_area` and freeze protection. The
loop does not reuse `read_slot`: that buffers the whole length into a `KVec`,
which for a whole slot is a megabyte.

`check_range` is **wrong for this opcode** and is not called. It reads `off` as
a within-slot offset, and on `READ` and `WRITE` `off` is a file offset. Both
open-code their own bounds check instead. The `Sqe::off` doc comment now lists
every per-opcode meaning, because that collision is the one a second
implementer meets first.

**`held_slot` is the line that matters.** It is the only thing that frees the
slot when a queued op is cancelled, so a missing `KORU_OP_WRITE` there shows up
nowhere except as every later op on that index getting `-EBUSY`, with nothing
saying why. The check asserts it directly by reusing the slot after a cancel.

**A non-zero `off` on an unseekable file is refused**, by `FMODE_LSEEK`.
`rw_verify_area` rejects only a *negative* offset, so without this the field
would be silently ignored on a pipe or a socket rather than refused. Nothing
can reach it yet — `OPEN` takes regular files only — but T19 can, and T37
writes to a socket through this opcode with `off` zero, which stays legal.

**No feature bit.** `WRITE` is unconditional: any kernel at this ABI version
has it, so there is nothing to probe and `KoruParams::features` stays zero.
`features` is for capabilities that can actually be absent.

`O_APPEND` is the wart. `kernel_write` inherits `IOCB_APPEND` from
`f_iocb_flags`, so on an appending handle the kernel appends and `off` is
ignored. koru's own open flags cannot set it; an adopted descriptor (T19) can.

#### What the errno cannot see

Three of the four guards in `check_writable` and `write_validate` are not
falsifiable by asserting a completion code, and two of those not at all yet.
Recorded because a check nobody has watched fail proves nothing.

- Delete the `FMODE_WRITE` guard and the read-only-handle test still passes:
  `__kernel_write_iter` has its own `WARN_ON_ONCE` returning the same `-EBADF`.
  What catches it is the taint gate, which goes 4096 to 4608, and the dmesg
  scan. So the guard's job is exactly `check_readable`'s — keeping the log
  clean — and the gate, not the assertion, is its oracle.
- Delete the negative-file-offset guard and **nothing** fails: `rw_verify_area`
  answers `-EINVAL` itself for a regular file, with no splat. It is kept
  because it refuses before spending a work item, and for symmetry with `READ`,
  whose guard is redundant in the same way. That is a cost argument, not a
  correctness one, and it should not be mistaken for a tested one.
- The `S_IFREG` and `f_op` shape guards cannot be reached at all: `OPEN` refuses
  every non-regular file, and a directory cannot be opened for writing. They
  become reachable at T19.

#### What was verified, and how

Six perturbations, each reverted. Four fail, and the two that do not are the
finding above.

- Drop `KORU_OP_WRITE` from `held_slot`. The cancelled write never releases its
  slot and the reuse probe fails.
- Hold the arena mutex across `write_at`. Lockdep reports the circular
  dependency and the dmesg gate fails the run, which is the same three-link
  cycle `READ` found in T10.
- Delete the `len` bounds. The zero-length case returns 0 and the oversized one
  returns 65537 — a write that ran one byte past its slot.
- Change `KORU_OP_WRITE` in one userspace mirror. `scripts/abi.sh` fails.
  Change it in **both** and the diff passes while every device test fails with
  `-EINVAL`, which is the clearest demonstration that the diff cannot see the
  kernel and that the device suites are what tie a mirror to it.

The fuzzer grew a `WRITE` arm and reaches it about five thousand times per
three-second run, with a few dozen succeeding — most handles in its pool are
read-only, and `-EBADF` is a fine outcome. **Its writable handle comes from a
dedicated scratch file and from nothing else.** Every other path it opens is
read-only, one of them `/etc/hostname`; a writable handle plus a random offset
would corrupt whatever it named. That sandboxing is a property of the test, not
of the kernel.

Opcode 7 used to be the "does not exist" probe in two rejection matrices, the C
check's `all_opcodes` and the Rust suite's `ALL_OPCODES`. Both now name
`KORU_OP_WRITE` and probe 8 instead. The fuzzer's per-opcode counters were
sized 8 with slot 7 as an "other" bucket, so they and their `& 7` masks had to
widen before the new opcode could be counted at all.

### Non-blocking, and the gate that moved

T18 added `KORU_O_NONBLOCK` and with it the first file koru can reach that is
not a regular file or a directory. Stdin and stdout are still out of reach
until `ADOPT_FD`, but the kernel rule they need is now in place.

**`RWF_NOWAIT` is not the mechanism.** `kiocb_set_rw_flags` returns
`-EOPNOTSUPP` unless `f_mode` carries `FMODE_NOWAIT`, and a `filp_open`'d FIFO
never has it: only `pipe(2)`, `sock_alloc_file`, eventfd, timerfd, signalfd and
userfaultfd set it. A tty never does, and `n_tty` looks at `f_flags &
O_NONBLOCK` and nothing else. So the mechanism is `O_NONBLOCK` at open time.
That works through `kernel_read` because `init_sync_kiocb` copies the `struct
file` pointer into the iocb, and the pipe code tests `ki_filp->f_flags`
directly. It does *not* work through `f_iocb_flags`, which encodes only
`O_APPEND`, `O_DIRECT` and the sync flags — `IOCB_NOWAIT` is never set on this
path.

**An empty FIFO with no writer reads as 0, not `-EAGAIN`.** The pipe code checks
for a missing writer before it looks at `O_NONBLOCK`, so end of file wins.
`-EAGAIN` needs a writer attached and the pipe empty. The obvious expectation is
the wrong one, and a test that asserts it passes for the wrong reason.

**The gate moved off `OPEN` onto `READ` and `WRITE`.** `do_open` used to refuse
anything but a regular file or a directory; now it refuses nothing, and
`check_readable` and `check_writable` admit a non-regular file exactly when it
was opened non-blocking. Three reasons. The done test needs a FIFO handle whose
`READ` is refused, which is impossible if `OPEN` refuses the FIFO. `ADOPT_FD`
will hand out handles that never went through `OPEN` at all, and one gate is
better than two. And a handle to a socket or a device grants nothing: the open
ran with the caller's credentials in the caller's context, so it is exactly what
`open(2)` would have given them. The visible change is that `OPEN` of a device
node now yields a handle instead of `-EINVAL`.

**koru never sets or clears `O_NONBLOCK` on a file it did not open.** It only
ever reads the bit. For an adopted descriptor the `struct file` is shared with
the rest of the process, so flipping it on stdin would change behaviour for
every other holder of that open file description. This is written into the
flag's own doc comment because T19 is where the temptation arrives.

`ENXIO` joined the errno table: a write-only non-blocking open of a FIFO with no
reader is the first way koru can produce it.

#### What was verified, and how

Six perturbations, each reverted. **Two of them fail as a hang rather than as an
assertion**, which is the point — a blocked kworker is what the gate exists to
prevent, and it cannot show up as a wrong value.

- Delete the non-blocking condition in `check_readable`. The `READ` of a FIFO
  handle opened without the flag blocks in a kworker and the run stops dead
  mid-section; the watchdog is the only thing that ends it.
- Stop translating the flag in `open_flags`. The section produces no output at
  all: the very first open, of a peerless FIFO, blocks inside `filp_open`. That
  is the stall itself, reproduced.
- Delete the `f_op` shape guard. The non-blocking directory `READ` still answers
  `-EINVAL`, because `__kernel_read` refuses it too — but it logs `kernel read
  not supported for file`, and the dmesg gate fails the run. Same shape as T17's
  `FMODE_WRITE` finding: the guard's job is the log, and the gate is its oracle.
- Delete the seekability guard in `read_validate`, then in `write_validate`. A
  `READ` from a FIFO with a non-zero `off` answers `-EAGAIN` instead of
  `-EINVAL`, and a `WRITE` returns a byte count, silently ignoring the field.
  **T17 had to record both as unreachable; they are falsified now.**
- Drop the bit from `KORU_OPEN_FLAGS_ALL`. Every open carrying it is `-EINVAL`.

The fuzzer learned the flag but was deliberately given no FIFO path. Its hostile
generator flips flag bits, so it could clear `O_NONBLOCK` and block a worker
inside `filp_open` for ever — a hang with no diagnosis, in the one place that
already runs for three seconds under eight threads. On the three regular files
it does open, the flag is a no-op, which is all the coverage the translation
needs.

### Adopting a descriptor

T19 added `ADOPT_FD`, which turns a descriptor the caller already holds into a
koru handle. With T17's `WRITE` and T18's non-blocking gate, stdout can now
reach the ring, which was the whole point of reopening the kernel.

An opcode rather than a fourth ioctl, so it inherits C1, E1, the `user_data`
echo, batching and the fuzzer's oracle without a line of new plumbing. The
descriptor travels in `off` rather than `handle`, which keeps "`handle` is a
koru handle except on `OPEN`" from gaining a third exception. `off` bounded at
`i32::MAX` before `fget` sees it, so `AT_FDCWD`-style magic numbers never reach
it. `O_PATH` needs no check of its own: `fget` is `__fget(fd, FMODE_PATH)` and
already returns NULL for those.

**Inline, and for a different reason from `OPEN`.** `OPEN` is inline because a
kworker would resolve and permission-check as root. `ADOPT_FD` is inline because
`fget` resolves against `current->files`, which in a kworker is the kthread's
table — a different fd table entirely, not a different privilege.

**The security statement is the opposite of `OPEN`'s, and stronger.** `fget`
performs no permission check, so **`ADOPT_FD` grants koru no authority the
submitting task does not already hold**. The task had the descriptor; it still
has it. The check asserts this the only way that means anything: a child dropped
to nobody is refused `/etc/shadow` by `OPEN` and **succeeds** at adopting a
descriptor its root parent opened on the same file, in the same test. If that
case ever starts failing, something has begun re-checking permissions at use
time.

**The owning reference, not the light one.** `LocalFile::fget` plus
`assume_no_fdget_pos`, never `fdget`. The safety condition holds because the
ioctl path takes `fdget`, not `fdget_pos`. A light reference is valid only while
the fd table cannot change, and this one outlives the ioctl by design: a kworker
on another CPU will use it. The check proves it by adopting, calling `close(2)`
on the descriptor, and reading through the handle anyway.

That property is asserted but **not falsified**, and the reason is worth
recording: the Rust abstraction exposes no way to build a light reference, so
the mistake cannot be written. `bindings::fdget` exists but returns a `struct
fd` the safe layer never converts. An un-writable mistake needs no perturbation.

#### `ELOOP`, and what its absence costs

Adopting a koru descriptor puts an `Arc<RingCtx>` into a handle table that lives
inside a `RingCtx`. `release` then never runs, the arena never frees, and the
module is permanently unloadable. **Comparing against our own file is not
enough**: two rings reach each other in two hops. The check is on `f_op`, which
every koru fd shares, and getting at the ring's own `f_op` is why the ring's
`&File` is now threaded from `ioctl` through `enter` and `submit` into
`dispatch`. Every `MiscDevice` callback already received it; koru had simply
been ignoring it.

Deleting that check does exactly what the argument predicts. The adoption
returns a handle, and **`rmmod` is then refused for the rest of the boot**: the
ring is unreachable by `release` because it holds itself.

#### Stdout is usable, and only because of T18

The deliverable is a child whose stdout is a pipe, adopting descriptor 1,
writing through the ring, and the parent reading the bytes off the other end.
**The pipe has to be created non-blocking**, and that is not an incidental
detail of the test: T18's gate refuses a `WRITE` to a non-regular file that was
not opened `O_NONBLOCK`, and koru never sets that bit on a file it did not open.
Making the pipe blocking is a one-character change and the child fails with the
write refused.

So a program whose stdout is a pipe or a tty cannot write through koru unless
whoever created that descriptor made it non-blocking. Redirected to a regular
file it works unconditionally, since regular files are always admitted. Phase 9
is unaffected, since the screen client creates its own socket, but T21's hello
world inherits this and it is the runtime's problem, not the kernel's.

#### What was verified, and how

Five perturbations, each reverted.

- Delete the `ELOOP` check. The own-ring adoption returns a handle instead of
  `-ELOOP`, and `rmmod` is refused afterwards.
- Delete the `i32::MAX` bound. The oversized descriptor reaches `fget` and comes
  back `-EBADF` instead of `-EINVAL`, which is the wrong answer to the wrong
  question.
- Delete the zero-field checks. All three of `len`, `slot` and `handle` stop
  being refused.
- Make the stdout pipe blocking. The child's `WRITE` is refused, which is T18's
  gate doing its job and the reason this task follows that one.
- Change the opcode in one userspace mirror. `scripts/abi.sh` fails.

The fuzzer grew an `ADOPT_FD` arm that names one of three things: a read-only
scratch descriptor, the fuzz ring itself, and a number that is no descriptor at
all, so `-ELOOP` is exercised thousands of times a run. **Never 0, 1 or 2.** An
adopted stdout plus the fuzzer's random `WRITE` would shred the check's own
output, and the failure would look like a corrupt test rather than a bug.

### Polling, and the third shape an op can have

T22 added `POLL_ADD`, and with it a shape neither inline nor deferred:
**armed**. Nothing is queued; the op puts a wait entry on the file's own
waitqueue and stops there, counting as in flight so `ENTER` sleeps for it and
`release` finds it. Everything below follows from one fact — the thing that
finishes it runs in a context that may do almost nothing.

#### The wake callback's rule, which is the `UserSlice` rule's sibling

The callback runs **with the waitqueue head's spinlock held**, and for a
socket fed by the network stack it runs in **softirq**. So it may not take a
koru lock, may not `complete`, may not `fput`, and may not touch the waitqueue
list it is being walked from. `kernel::sync::SpinLock` is plain `spin_lock`
with no irq-safe variant in this tree, so taking the ring lock there is an
irq-inversion, and lockdep says so.

What it may do is three things: filter the wake against the mask it asked for,
win a one-shot token, and `queue_work_on` the op's own work item. The kworker
then does the removal and the completion in process context, where all of that
is legal. This belongs beside "`ENTER` must never touch a `UserSlice` while
holding the ring `SpinLock`", and for the same reason: the context decides
what is allowed, not the call.

#### One token, four claimants

Four parties can want to finish an armed poll: the wake callback, the arming
`ENTER` itself when the file is already ready, a `CANCEL`, and `release`. Every
one of them must do exactly the same teardown — take the entry off the
waitqueue, reclaim one reference, unregister — and exactly one of them may.

So `PollState` carries a one-shot `Atomic<u32>`, `POLL_ARMED` until somebody
wins the `cmpxchg`. The winner owns the teardown; everyone else walks away.
That is what preserves the refcount rule Notes already records for `CANCEL`:
the arm leaks one reference — the credit `run` consumes — *before* it queues
the wait entry, because the callback can fire the instant the entry is on the
list and from softirq it could not take one of its own. The winner reclaims
that credit if `run` will never be called.

`CANCEL` needed a second arm for this. An armed poll is on no workqueue, so
`cancel_delayed_work` answers false and the old code would have reported
`-EALREADY` for an op that is merely waiting, for ever. Now the token decides,
and losing it means the wake has already queued the op — which is `EALREADY`
for the same reason a running op is.

#### `release` disarms before it drops anything

`cancel_all` disarms every armed poll **inside the `pending` lock and before
the registry is swapped out**, because the op's `ARef<File>` is the only thing
keeping the waitqueue head alive. Backwards is not a leak, it is a
use-after-free walking a freed list node from softirq — and it is a
use-after-free that only fires if something writes to the file afterwards,
which is why `koru_check pending` now leaves an armed poll behind and forks a
child to poke it once the ring is gone.

#### A file on no waitqueue must never be armed

The first version armed whenever the current mask did not match, and the fuzzer
found the hole in one run: eight SQEs consumed and never reaped. A regular file
has no `poll` method, so `vfs_poll` reports `DEFAULT_POLLMASK` and queues
nothing — and a poll for `RDHUP` alone, which that mask does not contain, would
then wait on a waitqueue that does not exist, holding its CQ reservation and
its in-flight count until the ring died.

So the rule is not "complete when ready" but **complete whenever nothing could
ever wake us**: `heads == 0` after the arm completes immediately with whatever
the mask says, even when that is an empty mask and `res` is 0. `res == 0` is
therefore a real answer — "this file cannot report what you asked" — and not
an error.

#### One waitqueue per arm, and a pipe is the reason

`poll_wait` may be called more than once per `poll`, and `pipe_poll` calls it
twice — `rd_wait` and `wr_wait` — for a pipe opened read-write. Supporting that
means io_uring's double-entry machinery, which multiplies the refcount rule
across two entries on the module's most delicate code. koru counts the calls
instead, queues only the first, and completes `-EOPNOTSUPP`. A FIFO opened for
reading, which is what a program actually polls, asks for one.

#### What the wake reports

The key a waitqueue passes is the mask that changed, and it may be null. So the
callback stashes what it was given and the kworker reports it; on a keyless
wake the kworker asks the file again, with a null `_qproc`, which is the
ask-only form of `vfs_poll`. Either way the answer is filtered to what the
caller asked for, plus `ERR` and `HUP`, which poll(2) reports whether or not
they were requested.

#### No blocking gate, unlike `READ` and `WRITE`

T18's rule — a non-regular file needs `O_NONBLOCK` — does not apply here and
must not. A poll never transfers anything, so it cannot wedge a kworker;
waiting is the entire point of it. `POLL_ADD` is the one op a blocking
descriptor is always welcome on, which is what makes it the answer to the
`EAGAIN` a non-blocking `READ` gives.

#### Testing a softirq wake needs a real socket

A `socketpair` does not do it: `unix_stream_sendmsg` calls `sk_data_ready` in
the **sender's own process context**, so an AF_UNIX wake looks exactly like a
FIFO's. A UDP datagram over loopback is what reaches the wake callback from
`handle_softirqs`, and that is the case the whole rule is about. Both are in
the check, labelled for what they are.

The fuzzer's `POLL_ADD` arm names only regular files and bad handles, so it
exercises the validation surface and never arms anything. That is deliberate:
an armed poll holds its CQ reservation until its event arrives, and the fuzzer
has no way to deliver one. It is the same exhaustion the plan gives as a reason
not to auto-arm on `-EAGAIN`.

#### What was shown to fail

Five perturbations, each applied and reverted. Three of them do not fail an
assertion — they take the machine down or say nothing at all, which is worth
knowing about this opcode.

- **Take the ring lock in the wake callback.** Every assertion still passes and
  the section reports `OK`. Lockdep reports `possible irq lock inversion
  dependency detected`, with `SOFTIRQ-ON-W` against `IN-SOFTIRQ-W`, and the
  dmesg gate fails the run. **Lockdep is the only oracle here.**
- **Call `complete()` from the wake callback.** The guest wedges outright,
  before lockdep gets to report anything: the double completion underflows the
  in-flight count in a kworker. A hang is a legitimate failure, but the
  lock-only perturbation above is the one that shows the rule.
- **Drop `remove_wait_queue` from the disarm.** KASAN reports a
  slab-use-after-free the moment the peer writes to the cancelled poll's FIFO,
  and the guest dies during the scan: the runner reports no verdict at all.
- **Make the token always succeed.** The cancel-versus-wake loop wedges the
  guest. The `poll` section alone still passes, because a cancel that disarms
  first leaves nothing for a wake to race — the race loop is what has teeth.
- **Skip the disarm in `cancel_all`.** `koru_check pending` plus its poking
  child gives `BUG: KASAN: slab-use-after-free in __wake_up_common_lock`.

### Stat, and the first result that does not fit in a CQE

`res` plus `extra` is sixteen bytes and a `kstat` is about a hundred and fifty,
so T23's `STAT` answers into the arena. That makes it the first opcode whose
result is data rather than a number, and the first to use all four SQE fields at
once for that purpose: `handle` names the file, `slot` and `off` name the
destination, and `len` is the caller's buffer size.

**`len` is version negotiation, not a request.** The kernel writes
`min(len, sizeof(KoruStat))` bytes and returns that count in `res`, so a binary
built against a shorter `KoruStat` asks for its own size and gets exactly that
prefix, while one built against a longer future struct asks for more and is told
how much it actually got. Nothing outside `[off, off + res)` is touched, which
is what lets the caller keep its own data in the rest of the slot.

**`off` must be a multiple of eight.** Every field is 64 bits, so an aligned
destination is what lets userspace read the struct in place rather than copying
it out; refusing an unaligned one is what makes that promise true rather than
usually true. This is the same reasoning T29's dirent header will need.

`check_range` is the right bounds check here, unlike on `READ` and `WRITE`:
`off` is a within-slot offset on `STAT`, as it is on `OPEN` and `CHECKSUM`. A
zero `len` is refused for the reason an empty `POLL_ADD` mask is — a stat that
reports nothing can only be a caller bug.

**Deferred, and therefore in `held_slot`.** `vfs_getattr` calls into the
filesystem and blocks on NFS and FUSE, so it may not run inline; holding a slot
across that window puts `STAT` in the same category as `CHECKSUM`, `READ` and
`WRITE`, and `OpWork::held_slot` is again the only thing that frees the slot
when a queued one is cancelled. That is now the third opcode to need that line
and the third time the symptom of forgetting it is every later op on that index
getting `-EBUSY` with nothing saying why.

**`KoruStat` is 256 bytes and is filled whole.** `READ`'s argument that the rest
of the slot is the caller's own data does not transfer: a partly filled struct
would make the caller read its own stale bytes as kernel-reported values. The
struct is built by a `#[derive(Default)]` literal in which every field is either
assigned or an explicitly zeroed `reserved` word, and then copied out as bytes,
so "partly filled" is not representable rather than merely avoided. Times are
second-plus-nanosecond pairs and device numbers are explicit major and minor, so
nothing on the wire is a kernel-internal encoding; `kstat` is translated field
by field, never transmuted.

`mode` is the one place a host value passes straight through. The `S_IF*` values
are identical on every Linux architecture — unlike `O_*`, which is why the open
flags are translated — so `KORU_S_IFREG` and friends are koru constants with
those values, and a binding needs no `<sys/stat.h>`.

#### The user namespace has to travel with the op

`kstat.uid` is a `kuid_t`, meaningful only through `from_kuid(ns, ...)`. In a
kworker `current_user_ns()` is init's, so a deferred stat translated there
reports the wrong numbers inside a container — wrong numbers rather than a
privilege escalation, so nothing catches it by accident. It is finding 3's quiet
sibling.

`OpWork` therefore carries an `ARef<Credential>`, taken from `current->cred` in
ioctl context, and the worker translates through `cred->user_ns`. The cred is
taken for **every** deferred op rather than only for `STAT`: an op resolves
everything it needs at submit time, a kworker has no route back to the
submitting task, and one `get_cred` beside the existing allocation and module
reference costs nothing measurable. Reading `current->cred` needs no RCU —
a task's creds are replaced only by that task — and holding the cred is also
what keeps the namespace alive, since `cred` holds a reference to it.

The ids are munged, exactly as `stat(2)` munges them: an id with no mapping
reports `overflowuid` rather than a raw `(uid_t)-1` that nothing else in the
system uses. `from_kuid_munged` and `from_kgid_munged` are used directly.

**This forced a kernel config change.** `vng --kconfig` leaves `CONFIG_USER_NS`
off, and with it off `from_kuid` and `from_kgid` are static inlines that bindgen
never emits — `from_kuid` survives only because `rust/helpers/task.c` happens to
wrap it, and there is no equivalent for `from_kgid`, so a gid could not be
translated from Rust at all. Worse for the check: every id maps one to one, so
the whole mechanism above would have had no way to be wrong. `CONFIG_USER_NS=y`
is now in `scripts/koru-debug.config`; it needs only `NAMESPACES`, which the
base config already has, and it turned out to bring the `_munged` variants into
the bindings too. The module now requires a kernel with `CONFIG_USER_NS`, which
every distribution kernel has.

The consequence for the check is the point: the namespace test is no longer the
"would fail in a container" comment the plan settled for. A child `unshare`s a
user namespace, maps `100 0 1` and `200 0 1`, and stats a root-owned file; koru
must report uid 100 and gid 200, and `fstat(2)` in the same child must agree.
`uid_map` is accepted because the single mapped id is the task's own; `gid_map`
needs `setgroups` set to `deny` first, because `unshare` has just taken away the
`CAP_SETGID` in the parent namespace that would otherwise permit it.

#### `extra`, and a mask that was almost untestable

`STAT` is the first opcode to set `Cqe::extra`, which carries the mask of fields
the kernel actually filled. The fuzzer's blanket `extra == 0` oracle became
per-opcode at the same time; leaving it blanket would have made the fuzz green
for the wrong reason, and making it per-opcode is what keeps it honest for every
opcode after this one.

The mask is koru's own bits, not `kstat.result_mask`, for the reason the poll
events and the open flags are koru's own: the wire format does not name host
constants, and statx's mask names fields koru does not carry. **The first
version of it was not falsifiable, and the perturbation is what found that.**
Twelve koru bits in `KoruStat` field order are a permutation of statx's twelve,
and a permutation maps the full set to the full set — so on a filesystem that
reports everything, passing `result_mask` straight through produced exactly the
same number as translating it, and the deliberately broken build passed.

The fix was to make the mask describe the whole struct rather than only the
fields statx has a bit for: `blksize`, `dev` and `rdev` are always filled by the
VFS and now have bits of their own, so `KORU_STAT_ALL` is fifteen bits and a
pass-through of statx's twelve fails three assertions at once. That is a better
ABI as well as a testable one — every `KoruStat` field is either covered by the
mask or reserved.

#### What was verified, and how

Nine perturbations, each applied and reverted. Eight fail; the ninth is the
finding below.

- **Drop `KORU_OP_STAT` from `held_slot`.** Nine assertions fail: the slot is
  never released, so everything after the first stat gets `-EBUSY`.
- **Translate in `current_user_ns()` instead of the carried cred.** Exactly one
  assertion fails, the namespace child's, with the verdict that says koru and
  `fstat(2)` disagreed.
- **Delete the eight-alignment guard.** The unaligned stat succeeds and returns
  256.
- **Delete the zero-`len` guard.** A zero-length stat returns 0 instead of
  `-EINVAL`.
- **Write `sizeof(KoruStat)` instead of `min(len, sizeof)`.** The short-`len`
  case returns 256 and the sentinel past `len` is overwritten — the dangerous
  half, since that is the caller's own data.
- **Stop the copy short of `reserved`.** Four assertions fail: the poison
  pre-filled into the slot reads back where zeros belong.
- **Pass `result_mask` through untranslated.** Three assertions fail — *after*
  the mask was widened. Before that it passed, which is recorded above.
- **Delete `check_range`.** The off-plus-len case writes sixteen bytes past the
  slot into its neighbour and reports success. The slot-index case still fails
  `EINVAL`, because `write_slot` finds no page there — so on this opcode only
  the range half of that guard has teeth.
- **Hold the arena mutex across `vfs_getattr`.** Nothing fails and lockdep says
  nothing. `READ`'s three-link cycle needs the inode rwsem, and neither
  `vfs_getattr` nor any `getattr` method takes it. The rule is still observed
  here — `write_slot` runs after the VFS call returns — but on this opcode it is
  inherited from `READ`, not independently tested. Do not mistake it for a
  tested guard.

The fuzzer grew a `STAT` arm and reaches it about 2,500 times per three-second
run, with a few dozen succeeding — the same handle-pool-limited rate `READ`,
`WRITE` and `POLL_ADD` get.

One assertion is weaker than the plan asked for. The plan wanted two concurrent
stats on one slot to produce one `-EBUSY`; two 256-byte stats on tmpfs almost
never overlap, so the check pairs a whole-slot `CHECKSUM` with a `STAT` instead,
which is the idiom `OPEN` and `READ` already use and tests the same property
deterministically.

### Path operations, and one shape for all of them

T24 added `TRUNCATE`, `UTIMES` and `READLINK`: the first ops that name a file by
path rather than by handle since `OPEN`, and the first to need `kern_path`.

**One encoding covers every path op**, and it is `OPEN`'s: the path is `len`
bytes at `off` in slot `slot`, with `handle` zero. An argument that does not fit
in the SQE follows the path in the same slot, at the first 8-aligned offset at
or after its end. `TRUNCATE`'s is a `u64` new length, `UTIMES`' is a
`KoruTimes`, and `READLINK` has none.

The plan proposed putting `TRUNCATE`'s new length in `off` instead, since `off`
is a file offset everywhere else. That was not taken. A file length has to be 64
bits, so `off` is the only field that can hold it, and spending it would leave
the path with no offset — a second path encoding, for one opcode, where every
path op after it reuses `do_open`'s recipe verbatim. One rule that covers five
opcodes is worth more than saving the caller an eight-byte store. What the plan
was right about is that `len` cannot be the truncate length; it just is not free
either.

`READLINK`'s answer **replaces the path it was given** and `res` is its length
without the NUL. An answer that does not fit in the rest of the slot is
`ENAMETOOLONG`, not a truncated path: `readlink(2)` truncates silently, and a
silently truncated path is a wrong path that looks like a right one.

**All three are inline, permanently**, for `OPEN`'s reason and with more force:
`vfs_truncate` and `vfs_utimes` permission-check with `current_cred()`, and
`kern_path` resolves a relative path against `current->fs`. In a kworker both
are init's. Finding 3 again, and T24 is where it finally got a regression test —
see below.

#### `kernel/koru_path.rs`, and what nothing checks

`linux/namei.h` is not in `rust/bindings/bindings_helper.h`, so `kern_path` and
every `LOOKUP_*` are missing from `bindings::` even though the symbol is a plain
`EXPORT_SYMBOL`. `do_delayed_call` is a static inline, so there is no symbol at
all and it is reimplemented in Rust over the bound two-field struct.

Everything of that kind lives in one file, each item quoting the header it came
from. **Nothing checks these**: no `static_assert` reaches a C declaration, and
the ABI diff cannot see the kernel. Re-reading that file against the source is
an obligation of every kernel bump, and it is the only such obligation in the
tree.

`vfs_truncate`, `vfs_utimes`, `vfs_get_link`, `path_put`, `mnt_want_write` and
`mnt_drop_write` all turned out to be in `bindings::` already, because
`linux/fs.h` is in the helper header. Only the namei half was missing.

Declaring `kern_path` needs `#[allow(improper_ctypes)]`: `struct path` reaches a
bindgen struct this config leaves empty, and the `bindings` crate allows the
same lint crate-wide for the same reason.

#### Two guards, and the one the plan asked for that is not needed

`Lookup` holds a `struct path` and calls `path_put` on drop; `Link` holds
`vfs_get_link`'s answer and makes its delayed call on drop. Both exist because a
`?` that skipped the teardown would leak silently, which is one of the few
places Rust's `Drop` genuinely earns its keep here.

The plan also asked for a `mnt_want_write`/`mnt_drop_write` guard, warning that
an early return skipping the drop pins the filesystem against read-only remount
until reboot. **No T24 op needs it.** `vfs_truncate` and `vfs_utimes` are the
wrappers that take and drop the write count themselves — that is the difference
between them and the `vfs_mkdir` family — and `READLINK` writes nothing. Writing
the guard now would have meant a `Drop` impl no test could reach. It belongs to
T26, where `vfs_mkdir` and `vfs_symlink` need it and the write-count balance
assertion the plan describes can actually fail.

#### kmemleak is the wrong instrument for `do_delayed_call`

The plan expected 2,000 readlinks with the delayed call dropped to make kmemleak
report. It does not, and the reason is worth keeping: `shmem_put_link` is
`folio_mark_accessed` plus `folio_put`, and `page_put_link` is the same shape.
The delayed call releases a **reference**, not an allocation. The page stays
referenced — for ever — so kmemleak, which looks for unreferenced objects, sees
a perfectly healthy page.

What does see it is `MemFree`, once each symlink is unlinked so its page would
otherwise be freed. The check makes 4,000 distinct page-backed symlinks, reads
each and unlinks it: correct, `MemFree` falls by about 5.9 MB of ordinary churn;
with the delayed call dropped, by about 22 MB — the same churn plus 4,000 pages.
The threshold is two kilobytes per link, which sits between them with room to
spare.

**The probe only works outside an overlay.** `/tmp` in the guest is an overlayfs
whose upper layer is tmpfs, and there the leak is invisible: with the delayed
call dropped the measurement is indistinguishable from a correct run. The links
therefore live in `/run`, which is plain tmpfs. A test that cannot fail where it
runs is worse than no test, and this one nearly was one.

The target has to be longer than tmpfs's `SHORT_SYMLINK_LEN` of 128 bytes, or
the symlink is stored inline, `simple_get_link` arms no delayed call, and
dropping the call changes nothing.

#### What was verified, and how

Seven perturbations, each applied and reverted, all seven fail.

- **Drop `do_delayed_call` from `Link`'s drop.** The `MemFree` probe fails, at
  22 MB against a 8 MB threshold. kmemleak still reports nothing, which is the
  finding above.
- **Drop `path_put` from `Lookup`'s drop.** The same probe fails: a leaked
  dentry reference keeps the unlinked inode, and its page, alive.
- **Give `READLINK` `LOOKUP_FOLLOW`.** Six assertions fail. The two-deep symlink
  is what separates "followed" from "not followed"; one link would have returned
  the same answer either way.
- **Take `LOOKUP_FOLLOW` away from `TRUNCATE`.** Truncating through a symlink
  gives `-EINVAL`, because the symlink itself is not a regular file.
- **Defer `TRUNCATE` to the workqueue.** The unprivileged child's truncate of a
  root-owned file succeeds instead of `-EACCES`. This is the plan's most
  valuable assertion and the first regression test the inline-because-of-creds
  rule has ever had for anything but `OPEN`.
- **Replace the `ENAMETOOLONG` guard with truncation.** `READLINK` returns a
  63-byte prefix of a 143-byte path and calls it success.
- **Delete the argument bound in `arg_offset`.** `TRUNCATE` reads its new length
  out of the *next slot* and succeeds. This one needed the test fixed first: the
  original case put the argument exactly at the slot end, which is legal, and
  the one after it was refused by the path scan rather than the bound. The
  assertion now names a real eight-character path so only the bound can refuse.

The creds child also distinguishes two refusals the VFS gives for `UTIMES`:
named times need ownership and give `-EPERM` from `setattr_prepare`, while both
nanoseconds set to `UTIME_NOW` is a touch, which needs only write permission and
gives `-EACCES`. Asserting one of them would have passed for the wrong reason.

The fuzzer grew an arm for all three, reaching each about 1,600 times per run
with a few hundred succeeding. **It names only its own scratch file, its own
symlink and a path that resolves nowhere.** A random path reaching `TRUNCATE`
would destroy whatever it named; that sandboxing is a property of the test, not
of the kernel, and it is the most important line in the fuzzer.

### Stat by path, and the first inline op with an `extra`

T25's `STATX_AT` is T23's answer reached by T24's encoding: `kern_path` with
`LOOKUP_FOLLOW`, the same `vfs_getattr` and the same `KoruStat`. The two
opcodes now share one `getattr` helper, which takes a `struct path` and a
`Credential` and returns the filled struct plus `kstat.result_mask`. `STAT`
hands it `file->f_path` and the cred its `OpWork` carried; `STATX_AT` hands it
the `Lookup`'s path and `current_cred()`, because it runs in the submitting
task and there is nothing to carry.

**The answer replaces the path**, at `off` in the same slot, exactly as
`READLINK`'s target does. One claim covers both halves and the path is consumed
into a `KVec` before anything is written, so the overlap raises no
acquisition-order question. The alternative — writing the struct to the
argument offset after the path — would have kept the path, and cost the caller
256 bytes of slot it did not ask to spend.

**There is no version negotiation here, because `len` is the path's length.**
`STAT` spends `len` on the caller's buffer size; this opcode cannot, so it
writes the whole 256 bytes or refuses with `-EINVAL`. That is safe for the same
reason `KoruStat`'s size is an assertion rather than a hope: a later field comes
out of `reserved`, so every binary at this ABI version agrees on 256. A prefix
would be worse than a refusal — the caller would read its own path bytes back
as fields.

The two guards that follow from that are `off % 8 == 0`, for `STAT`'s reason,
and `slot_size - off >= sizeof(KoruStat)`. The second is the only thing between
a destination near the slot end and a write into the **next slot**: the arena's
pages are contiguous, so `write_slot` finds a page there and succeeds. That is
the same shape as T23's finding that only the range half of `check_range` has
teeth on a stat.

`dispatch` now returns `(res, extra)` rather than `res`. `STATX_AT` is the
first opcode to complete inline *and* have something to say in `extra`, and
until T25 the submit loop could only ever post `extra` 0 for one. The tuple is
`run`'s, so the two completion paths now say the same thing the same way.

#### What was verified, and how

Seven perturbations, each applied and reverted, all seven fail.

- **Defer `STATX_AT` to the workqueue.** The unprivileged child's stat of a
  file inside a root-only directory succeeds instead of `-EACCES`, in both
  suites. This is the plan's own done test and finding 3's second regression
  test after T24's.
- **Delete the eight-alignment guard.** The unaligned stat succeeds and returns
  256.
- **Delete the room check.** A stat eight bytes from the slot end returns 256
  and writes 248 of them into the neighbouring slot, reporting success.
- **Drop the slot claim.** The `STATX_AT` behind a whole-slot `CHECKSUM`
  succeeds where it owes `-EBUSY`.
- **Take `LOOKUP_FOLLOW` away.** The two-deep symlink stats as a symlink rather
  than as the regular file at the end of it. One link would have been enough
  here, unlike `READLINK`, but two costs nothing and matches T24's case.
- **Post no mask in `extra`.** The "every field but btime was reported"
  assertion fails — which is what says the inline path really does carry an
  `extra` now.
- **Drop the zero-`handle` guard.** A `STATX_AT` naming both a path and a
  handle succeeds; every path op owes that rejection.

The fuzzer's arm reaches it about 2,300 times per three-second run with about
750 succeeding — the highest success rate of any path op, since a stat of an
existing path almost always works. It names the same three sandboxed paths the
T24 arm does. Its `extra` oracle is the `STAT` one, now shared by the two
opcodes, and the per-opcode counters stopped being indexed by `op & 15`: with
fifteen opcodes an unknown one aliased into a real bucket, which could have
satisfied the reached-once check for an opcode nothing ever submitted.

### Creating things, and the guard the plan kept asking for

T26 added `MKDIR` and `SYMLINK`, the first ops that make something rather than
read or change it. Both are `start_creating_path`, then `vfs_mkdir` or
`vfs_symlink`, then `end_creating_path` — the sequence `filename_mkdirat` and
`filename_symlinkat` are, with our own path copy in front of it.

**The `mnt_want_write` guard is not needed here either.** The plan moved it from
T24 to T26 on the grounds that `vfs_mkdir` and `vfs_symlink` do not take the
write count themselves. They do not — but `start_creating_path` does, inside
`filename_create`, and `end_creating_path` drops it along with the parent's lock
and the path reference. So the count is balanced by the same two calls that
balance everything else, and a separate guard would be a second, unbalanced one.
It belongs at T27, where `start_removing_path` is unusable and the sequence is
hand-assembled. This is the second time the plan has asked for that guard one
task too early; the instrument for it, though, is real and is described below.

**`Creating` is the guard that is needed**, and it holds three things at once:
the parent inode's lock, the mount's write count and the path reference. A `?`
between `start_creating_path` and `end_creating_path` would leave a directory
locked for ever, which is not a leak but a hang.

**`vfs_mkdir` may return a different dentry**, and it is that one, not the one
it was given, that `end_creating_path` must unlock — hence `Creating::replace`,
called on every path out of `vfs_mkdir`. Two halves of that rule behave very
differently here. The *success* half is unreachable in this VM: tmpfs and
overlayfs both return `NULL`, so the dentry never actually changes, and
perturbing the code to pass the original one changes nothing. Only a filesystem
whose `->mkdir` splices an alias would tell the difference. The *error* half is
reachable and sharp: on failure `vfs_mkdir` has already unlocked and dropped the
dentry, so passing that one on unlocks an inode nobody holds — and the check
does reach it, because `vfs_mkdir` is where an unprivileged create in a
searchable-but-unwritable directory is refused.

**`SYMLINK` carries two paths in one slot**, `len` covering both and the NUL
between them, target first as in `symlink(2)`. That is a different parse from
`copy_path`'s, not a relaxation of it: exactly one NUL may fall inside those
bytes, at neither end, and the second half gets a NUL of our own. The plan's
alternative — `off` as a second length — would have collided with `off`'s
within-slot meaning, which is the same argument T24 settled for `TRUNCATE`.

`MKDIR` carries its mode in `handle`, free on that opcode exactly as `OPEN`'s
flags are. The mask is `0o1777`, which is all `vfs_prepare_mode` keeps of a
requested directory mode; `S_ISUID` and `S_ISGID` are rejected rather than
silently dropped, as an unknown open flag is. The VFS applies the umask — the
*submitting* task's, which is another thing that would be wrong in a kworker.
**This retires the reason Notes gives for having no `O_CREAT`**: `handle` can
carry a creation mode after all. `OPEN` still has none, because on that opcode
`handle` is spent on the flags.

`delegated_inode` is NULL throughout. `try_break_deleg` with NULL takes no
reference and returns `-EWOULDBLOCK`, so there is no `iput` bookkeeping and no
retry loop; the cost is that an NFS delegation surfaces as `-EWOULDBLOCK`
instead of being broken.

#### What was verified, and how

Six perturbations, each applied and reverted. Five fail; the sixth is the
finding below.

- **Defer `MKDIR` to the workqueue.** The unprivileged child creates a directory
  inside a root-only one, and the sticky-bit mode comes back wrong because the
  kworker's umask is not the submitter's. Creating something as root for a
  caller who may not is the worst failure mode in this plan, and it is one line
  away at every one of these opcodes.
- **Pass `vfs_mkdir` the dentry it was given rather than the one it returned.**
  The guest hangs with no output, on the creds child's refused create: the
  parent inode is unlocked twice. A hang rather than an assertion, like two of
  T18's.
- **Leak one `mnt_want_write` per `MKDIR`.** Exactly one assertion fails, and
  only the last one: the tmpfs the heavy loop mounts refuses to go read-only
  with `EBUSY`. Nothing else anywhere in the check notices — no splat, no leak
  report — which is what makes that assertion the only instrument there is.
- **Delete the mode mask.** Three assertions fail; `S_ISUID` is accepted and
  silently dropped by the VFS, which is exactly the outcome koru refuses.
- **Accept an empty half of the pair.** A symlink to the empty string is
  created and reported as success.
- **Swap the two paths.** Six assertions fail, the creds child included.
- **Accept a second interior NUL, splitting at the first.** *Nothing fails.*
  Whatever the split leaves behind still reaches
  `CStr::from_bytes_with_nul`, which refuses an interior NUL with the same
  `EINVAL` — so the rule is subsumed by the conversion and cannot be falsified
  by errno. It stays because a parse should state its own rule rather than
  inherit it from a later step, but it is not a tested guard. The two-NUL case
  in the check passes for the conversion's reason, not for the rule's.

The heavy phase mounts a tmpfs of its own and runs 2,000 `MKDIR`-and-`rmdir`
plus 2,000 `SYMLINK`-and-`unlink` pairs on it before the remount. Its own mount,
because the write count a shared filesystem carries is everybody's.

#### The fuzzer's sandbox was never what it claimed

T24 recorded that the fuzzer "names only its own scratch file, its own symlink
and a path that resolves nowhere", and called that the most important line in
it. It was not true, and T26 is where that showed: `MKDIR` started creating
directories called `/tmp/koru-check-patte` — a 21-byte prefix of the check's own
pattern file, which is what `/tmp/koru-fuzz-dir/c3` truncates to.

Two holes, both older than T26 and both invisible while no opcode created
anything:

- **A generator writes the path into the slot as a side effect**, and a batch of
  eight SQEs shared one path slot per thread. The last generator to run decided
  what every path op in that batch would read, cut to each one's own `len`. The
  same hole let `TRUNCATE` name the pattern file, which is destructive, not
  merely untidy.
- **The hostile generator mutates `slot`, `off` and `len`**, and a random opcode
  byte can land on a path op. Either one points it at whatever another thread
  last wrote.

The fix is structural rather than statistical, which is what the plan demanded.
Every path op is now generated by one function, `gen_path`, which writes the
path itself and sets the exact `(slot, off, len)` for it; the hostile generator
regenerates any SQE that ends up being a path op, so a mutated field can never
survive on one. Each SQE in a batch gets its own private path slot — the arena
grew to `8 + NWORKERS * BATCH` slots for that — and `READ`, `WRITE` and
`CHECKSUM` pick only from the eight shared ones, so nothing can overwrite a path
while the kernel is copying it. The hostile slot values are all past
`slot_count` for the same reason.

What that costs is hostile coverage of the path ops' own fields, which the
deterministic rejection matrices carry instead — they cover every field of every
path op, and the validation is shared code. What it buys is that no fuzz run can
touch anything outside four names, provable by reading one function.

### Removing things, assembled by hand

T27's `UNLINK` and `RMDIR` are the first path ops with no wrapper to call.
`start_removing_path` is declared in namei.h but not exported, and the exported
`start_removing_user_path_at` takes a `char __user *`, which a module holding a
kernel string cannot use. So the sequence is `filename_unlinkat`'s, written out:
split the path, `kern_path` the parent, `mnt_want_write`, `start_removing`,
`vfs_unlink` or `vfs_rmdir`, then unwind in reverse. `end_dirop` is in
`bindings::` because it is declared in fs.h rather than namei.h — the one lucky
break in `koru_path.rs`.

**`start_removing` is the right door**, not `start_dirop`: it calls
`lookup_one_common`, which computes the name's hash with `full_name_hash`,
rejects an empty name, `.`, `..` and anything containing a separator or a NUL,
and checks `MAY_EXEC` on the parent — all the work `filename_parentat` would
have done and none of it available to us otherwise. The `qstr` we hand it
carries `len` and `name`; `hash` stays zero because that call fills it in.

**The `mnt_want_write` guard finally lands**, three tasks after the plan first
asked for it. T24's ops call `vfs_truncate` and `vfs_utimes`, which take the
count themselves; T26's call `start_creating_path`, which does the same. A
removal has no such wrapper, so the count is ours, and an unbalanced one is
invisible until a filesystem refuses to go read-only.

**Four guards unwind in declaration order reversed**, which is what Rust gives
for free and what the C does by hand with `goto`: the dirop unlocks the parent,
then the victim's `iput`, then the write count, then the path. `UNLINK` holds
that inode reference across `end_dirop` for the reason `filename_unlinkat`
states in its own comment — the last `iput` truncates, and truncation must not
happen under the parent's `i_rwsem`.

#### Our own split, and an errno the syscalls do not give

`filename_parentat` classifies the last component and its callers act on that:
`rmdir` answers `-ENOTEMPTY` for `..`, `-EINVAL` for `.` and `-EBUSY` for the
root, while `unlink` answers `-EISDIR` for all three. koru splits the path
itself, so it owes its own answer, and that answer is **`-EINVAL` for all four
cases** — empty, `.`, `..`, and a trailing separator, which leaves the last
component empty. Naming the directory above you is a caller bug, not an outcome.

Deleting that check does not make the removals succeed: `start_removing` refuses
the same four with `-EACCES`. That is exactly why the plan asked for the
assertion to fail *differently* rather than not at all, and it does — six
assertions turn from `-EINVAL` into `-EACCES`, or `-EROFS` for the root, whose
parent is itself.

A path with no separator resolves against the submitting task's working
directory, which is a second reason these ops are inline: a kworker's `cwd` is
the init root. The check has an assertion for that, and it fails with `-ENOENT`
the moment the op is deferred.

#### The read-only test that tested nothing

The first version of the `mnt_want_write` test mounted a tmpfs, remounted it
read-only and asserted `-EROFS`. It passed with the guard deleted. `IS_RDONLY`
is `sb_rdonly`, and `inode_permission` refuses `MAY_WRITE` on a read-only
superblock all by itself — so the superblock case never reaches the mount's
write count at all.

What isolates the guard is a **read-only bind mount** of a writable filesystem:
the inode is perfectly writable, `inode_permission` is content, and only
`mnt_want_write` refuses. The check makes one, asserts `-EROFS` through it, and
then removes the same inodes through the writable mount to show the mount was
the only thing that objected. With the guard deleted the read-only removal
succeeds — a read-only bypass, which is what that assertion is for.

#### What was verified, and how

Six perturbations, each applied and reverted. Five fail; the sixth is the
finding below.

- **Delete the non-normal last-component check.** Six assertions fail, each with
  a different errno rather than a success, as described above.
- **Drop `mnt_want_write`.** Five assertions fail: the read-only bind mount is
  bypassed and both removals succeed through it.
- **Defer `UNLINK` to the workqueue.** The unprivileged child removes a file
  from a directory only root can write, and the bare-name case fails with
  `-ENOENT` because a kworker's working directory is not the submitter's. Two
  independent signals for one mistake.
- **Drop `LOOKUP_DIRECTORY` from the parent lookup.** A path through a regular
  file gives `-EACCES` from `lookup_one_common` instead of `-ENOTDIR`.
- **Call `vfs_rmdir` for `UNLINK`.** Eleven assertions fail.
- **Drop the inode reference across `end_dirop`.** *Nothing fails*, and lockdep
  says nothing. It is a lock-hold-time property — the truncation happens under
  the parent's rwsem instead of after it — not a correctness one, so no errno
  and no instrument in this tree can see it. The code keeps it because the VFS
  keeps it and says why; it is an untested guard, like the arena-mutex rule on
  `STAT`.

The fuzzer grew an arm for both, on the same `FUZZDIR` names its creates use, so
the two directions race each other and the janitor. It also names `.`, `..` and
a trailing separator there, which only our own check refuses. The janitor slowed
from a sweep every millisecond to every five to twenty, because at the old rate
it removed everything before `UNLINK` or `RMDIR` could find it.

### Rename, and two mounts of one filesystem

T28's `RENAME` is the last path op and the only one that locks two directories
at once. `start_renaming` hashes and permission-checks both names, then
`lock_rename` takes both parents — and the superblock's `s_vfs_rename_mutex`
when they differ, which is what makes the ordering somebody else's problem. The
`Renaming` guard's drop is `end_renaming`, which unlocks both and drops three
references.

It reuses T26's two-path parse, old first then new, as `rename(2)` takes them,
and T27's `split_path` on each half — so a last component that is empty, `.`,
`..` or followed by a separator is `EINVAL` on both sides.

**One `mnt_want_write`, not two**, which is where the plan was wrong. It asks
for the write count on both mounts; after the cross-mount rejection there is
only one mount, and `filename_renameat2` takes only `old_path.mnt` for exactly
that reason.

#### The cross-mount check, and the test that nearly did not test it

The plan calls for "a cross-mount rejection of `-EXDEV` before anything
starts", and the first version of the check renamed between two *filesystems* —
a tmpfs and the root — and asserted `EXDEV`. **It passed with the check
deleted.** `lock_rename` answers `-EXDEV` itself when the two parents have no
common ancestor, which two superblocks never do.

What our check alone catches is **two mounts of one filesystem**: a bind mount,
where `lock_rename` is perfectly happy because there is one superblock, and only
`old_path.mnt != new_path.mnt` says no. That is not a formality. Without it the
write count would be taken on one mount while the rename wrote through the
other, which is T27's read-only-bind-mount bypass reached from a second
direction. The check now makes the bind mount, asserts `EXDEV` across it, and
then renames the same two names through a single mount to show which half
refused.

This is the second time in three tasks that a test of a mount-level rule passed
for a superblock-level reason. The rule generalises: **a mount-level guard can
only be tested with two mounts of one filesystem**, because everything else the
VFS refuses on its own.

`EXDEV` joins the errno table as `Kind::Unsupported`, beside `ENOTTY`, `ENOSYS`,
`EPROTO` and `EOPNOTSUPP`. Braam's fifteen kinds are fixed, so the question was
only which one: the two paths are each perfectly valid, and what is unsupported
is moving between filesystems — a program reading "unsupported" falls back to
copy-and-delete, which is the right thing. `Invalid` would have suggested fixing
an argument that is not wrong.

#### What was verified, and how

Four perturbations, each applied and reverted, all four fail.

- **Delete the cross-mount check.** The rename across two mounts of one
  filesystem succeeds, as described above.
- **Swap the two paths.** Fifteen assertions fail, the creds child included.
- **Never call `end_renaming`.** The guest hangs with no output: both parent
  directories stay locked for ever, so the next path op in either of them
  blocks in D state. A hang rather than an assertion, as in T26.
- **Drop `mnt_want_write` from the rename.** The read-only bind mount is
  bypassed and the rename succeeds through it. T27's guard, T28's use of it,
  and the same instrument.

The creds child renames inside the searchable-but-unwritable directory, and the
heavy loop renames each object before removing it, so the rename's own write
count is on trial with the other four opcodes' in the read-only remount.

The fuzzer's arm renames between the same `FUZZDIR` names its creates and
removals use, so all three race each other; `EXDEV` never comes up there, and
the deterministic matrix owns that case.

### Reading a directory, and the first shared state koru serialises

T29's `READDIR` is the last kernel opcode and the only one whose work is done by
a **callback the VFS calls back into us**. `iterate_dir` takes the directory's
`i_rwsem` for read and calls the actor once per entry.

**The new invariant, plainly: `READDIR` is the first opcode that mutates shared
per-file state, and it is serialised per handle for that reason.** `iterate_dir`
reads and writes `file->f_pos` and ignores `ctx->pos`, so `READ`'s
caller-owned-offset escape is not available: two concurrent reads of one handle
would interleave one position and lose or duplicate entries. The handle table
grew a busy flag, claimed at submit in ioctl context and released on every
completion path, so the second one gets `-EBUSY`. The slot bitmap's argument
transfers verbatim — userspace naming one resource twice is the kernel's problem
to refuse, not userspace's to avoid.

The claim is taken *before* the slot claim and given back if the slot fails, and
`OpWork::held_handle` sits beside `held_slot` so that `run`, `CANCEL` and the
`-EBUSY` unwind all release it. That is T17's refcount trap in its third
costume, and the check has an assertion for each of the three paths.

**A `CLOSE` during a `READDIR` leaves the entry claimed but empty**, so `insert`
skips a busy entry even when its file is gone. Without that the claim would
outlive the file and be given back against whatever `OPEN` reused the index —
a flag pointing at the wrong object, which is the same shape as a stale handle
and is what generations exist to prevent.

#### Three rules for the callback, and the one the plan got wrong

The actor runs under the directory's rwsem, in a kworker, so:

- **It allocates nothing.** The output buffer is `KVec`-allocated to the full
  budget before `iterate_dir`, and running out of room is `checked_add` and a
  comparison returning false — which is exactly what false means to
  `iterate_dir`, so there is no second channel to get wrong.
- **It cannot panic.** A Rust panic unwinding into C is `BUG()` on this kernel.
  Every fallible step returns false or skips the entry; there is no `?`, no
  `unwrap`, and no bounds-checked indexing — the writes go through
  `copy_nonoverlapping` after the arithmetic has already proved they fit.
- **It touches no koru lock**, and the arena write happens after `iterate_dir`
  returns.

The plan says that last one is "Notes' lockdep cycle reached from the other
end", and asks for a circular-locking report from taking the arena mutex inside
the callback. **That is not what happens.** Perturbed that way, the whole check
passes and lockdep says nothing, and the reason is worth keeping: the cycle
needs three edges, and the arena mutex has no outgoing edge to the VFS any more.
`mmap` gives `mmap_lock → arena`; a filesystem read gives `i_rwsem → mmap_lock`;
T10's bug gave `arena → i_rwsem` and was fixed. Adding `i_rwsem → arena` to the
first two is acyclic.

Perturbing *both* halves — the arena mutex in the callback and T10's bug
restored — does produce the report, and it is worth reading:

    kworker is trying to acquire (&sb->s_type->i_mutex_key)
      at netfs_start_io_direct, but already holds (koru.rs:418)
    -> #2 (koru.rs:418): koru's mmap, under mmap_region
    -> #1 (&mm->mmap_lock): gup_fast_fallback, under vfs_read

So the rule survives, but as a different rule: **do not add the second edge of
an inversion whose first edge is one mistake away.** It is a latent inversion,
not a live one, and the arena mutex is not otherwise held across anything that
can reach the VFS. That makes it an untested guard in the same sense as the
`STAT` one, and the entry above is the evidence that it is a real rule rather
than a superstition.

#### The entry format, and what `linux_dirent64` gets wrong

`KoruDirent` is `linux_dirent64` with its annoyances fixed, since there is no
compatibility to keep: a gap-free, **eight-aligned** 24-byte header — inode,
cookie, record length, name length, type — then the name, a NUL and padding to
eight. Eight-aligned is the point: every libc copies a `linux_dirent64` out of
its buffer because `d_ino` and `d_off` land on two-byte alignment, and here they
do not. `namelen` is explicit rather than implied by `reclen` minus a header
size, and the name is NUL-terminated anyway, because a C caller should not have
to build a string to call `open`. **`namelen` is authoritative**; the NUL is a
convenience.

**The cookie is `d_off`, and `d_off` does not mean what its name suggests.** The
offset the actor is handed is the position of the entry it is being given, and
`filldir64` stores it into the **previous** record — so a record's `d_off` is
where to seek to get the entry *after* it. The last record's cookie comes from
`ctx.pos` once `iterate_dir` has returned. koru copies that convention exactly,
including the back-patching, because a per-entry cookie that meant "this entry"
would make resumption re-deliver the entry the caller had already consumed. The
check asserts the distinction directly: resuming from entry zero's cookie must
return entry one first.

`res` is the total bytes written and `res == 0` is the end of the directory,
mirroring `READ` — which makes a budget too small for even the first entry a
trap, because it would otherwise look like the end. That case is `-EINVAL`, and
`getdents64` answers the same way for the same reason.

A name containing a NUL or a separator means a corrupt filesystem. koru skips
the entry and sets `KORU_CQE_F_SKIPPED` in the CQE rather than truncating
silently — the first use of `Cqe::flags`, which makes the fuzzer's shape oracle
per-opcode for `flags` as T23 made it for `extra`. No filesystem in this tree
can produce such a name, so that path is untested.

#### What was verified, and how

Five perturbations, each applied and reverted, all five fail.

- **Drop `KORU_OP_READDIR` from `held_slot`.** Ten assertions fail.
- **Forget the handle on the cancel path.** One fails, and it is the one that
  exists for it: the handle is still busy after the cancel.
- **Drop the per-handle claim entirely.** Two concurrent reads both succeed.
  The plan expected this to show up as duplicated or missing entries rather
  than as an errno, but the claim is taken at submit in ioctl context, which
  the submit lock already serialises, so exactly-one-`EBUSY` is deterministic.
  The content assertion is there as well, and catches a claim that is taken and
  never returned.
- **Return 0 for a budget too small for one entry.** The caller cannot tell a
  short buffer from the end of the directory.
- **Make the cookie name its own entry instead of the next.** Resumption
  re-delivers the entry the caller already had.

The sixth is the plan's own, recorded above: the arena mutex in the callback is
benign on its own and needs T10's bug beside it to close a cycle.

The check builds a directory of 500 entries and reads it with a 4 KB budget,
which takes four rounds, comparing the result as a **set** against `readdir(3)`
— name, inode and type — with dot and dot-dot included. The fuzzer opens the
directory it creates and feeds the last cookie any thread saw back in as a
random `off`, so a bogus resume position is exercised too.

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

**`-EALREADY` is reachable, and is now reached.** It went unobserved in 9,000
attempts across three race loops, including one against a `CHECKSUM` over a
whole slot, which was the longest-running op there was. The window is the span
between the worker clearing the pending bit and `run()` unregistering; a cancel
otherwise either arrives while the op is still queued, or after it has fully
completed.

What changed is the slot size. The integrated check's shared ring uses 64 KB
slots rather than 4 KB, so a whole-slot `CHECKSUM` runs sixteen times longer and
the window opens wide enough to hit: one to three times per thousand rounds,
reliably enough that a thousand rounds find it on essentially every run. The
unregister-after-complete ordering is therefore tested rather than merely
reasoned, and so is the half of the refcount rule that depends on it. See the
consolidation section below for the breakage that confirms it.

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

**This inverted a T6 assertion.** T6 had asserted that `rmmod` was
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

**Observing a collision is racy, and the test used to assume it was not.** Two
`CHECKSUM`s naming one slot are dispatched one after the other inside a single
submit loop, but the winner's kworker can run on another CPU and post its
completion — releasing the slot — before the loser is dispatched. Both then
succeed and an exclusive-or assertion fails. It is rare: once in about thirteen
full runs, and never in twelve targeted ones, which is exactly the frequency
that gets mistaken for cosmic rays.

Three assertions in `sec_slots` made that assumption. They now retry, up to
sixty-four times, and assert that a collision was observed at least once. That
is what the suite already does for every other racy property, and it keeps the
teeth: with `slot_try_acquire` forced to always succeed, no attempt collides and
all three fail. In practice the first attempt lands, so the loop costs nothing
and prints an attempt count only when it does not.

There is no deterministic fix available. Nothing userspace can submit holds a
slot for a controllable length of time: `DELAY_NS` holds none, and a blocking
`READ` would wedge a kworker. The window cannot be forced open, so asserting a
floor over repeated attempts is the honest shape for this claim.

### The tests live in the repo now

They used to sit next to the kernel tree, outside version control, which is how
all thirteen came to reference a path that no longer existed after the project
was renamed and stayed broken for four commits with nobody noticing. They are
under version control now and derive the repo root from their own location. A
directory rename is exactly the event that punishes an absolute path, and this
project has now been renamed twice.

### The tests were only advisory

The dmesg check at the end of every script printed splats without failing the
run. T10's lockdep deadlock therefore reported `PASS` on its first green run,
with the cycle sitting in the output above the pass line. The check now captures
that grep into a variable and gates the pass on it being empty, and the pattern
list includes `kernel read not supported for file`, which is a
`pr_warn_ratelimited` rather than a `WARN_ON` and so matched nothing before.

The lesson generalises past this bug: an assertion suite that passes proves the
code does what the test expects, and says nothing about what the kernel thinks
of it. On a debug kernel the kernel's own opinion has to be a hard gate.

The fences the script writes to `/dev/kmsg`, so a splat localises to a phase,
have to be excluded from that grep. Naming one of them after the section it
introduces made the scan match its own marker and fail every run.

### How a check earns trust

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

## One integrated check instead of thirteen

The kernel side is finished, so the suite's job changed. It is no longer
thirteen gates each proving one task complete; it is one dependency check that
has to run constantly while the userspace bindings are written. Thirteen cold VM
boots, an eight-second kmemleak sleep in nearly every script, and a ten-minute
default fuzz came to about twelve minutes, which is not a thing anyone runs in a
development loop.

It is now `test/koru_check`, one binary run by `scripts/check.sh` in one boot,
in about twenty-seven seconds. Almost every assertion survived. What was
wasteful was the scheduling, not the coverage.

### The ordering is the budget

kmemleak reports nothing about an object younger than `MSECS_MIN_AGE`, five
seconds, so the old scripts each paid a flat `sleep 8`. Paying that once is
still eight seconds of a twenty-seven second run, and paying it eleven times was
most of the old cost.

Instead the binary runs everything that allocates in bulk first, stamps
`KORU-HEAVY-END-MS` as its last line, and the script measures the age of that
stamp and sleeps only the shortfall below five and a half seconds. The tail
sections and the shell-driven `rmmod` races cover about four and a half seconds
of that window for free, because they were going to take that long anyway.

This makes the section ordering load-bearing in a way nothing else in the repo
is. **A new loop that allocates in bulk must go in the heavy phase.** Put one in
the tail and the single scan cannot see what it leaks, and nothing will say so.
The measured window is printed on every run so that a drift is visible rather
than silent.

The whole budget rests on that scan still working, so it was verified directly:
an unreachable 128-byte allocation per `OPEN` produces a wall of `unreferenced
object` reports and fails the run. Without that check the reordering would be an
untested optimisation of the one thing the suite is worst at seeing.

### Widen the window rather than repeat the roll

The race loops went from three thousand iterations to three or four hundred. The
compensation is that the shared ring uses 64 KB slots rather than 4 KB, so a
deferred `READ` or `CHECKSUM` over a whole slot runs sixteen times longer and
the interesting interleaving becomes the common case instead of a rare one.

Each loop then gates on having actually reached its arm, which none of the old
loops did. Measured on the dev kernel, with the gates set well below:

- `CLOSE` completes before the in-flight `READ` in 396 of 400 rounds; gate 350.
- Against an armed timer the cancel wins 300 of 300; gate 250.
- With `delay_ns` 0 the worker wins 284 of 300; gate 50.
- Against a whole-slot `CHECKSUM`, out of a thousand rounds, roughly 800
  cancelled and 180 already done; gates 150 and 20.

A loop that stops reaching its window now fails instead of passing silently.
That is coverage the three-thousand-iteration versions did not have, and it is
what the reduced counts were traded for.

**`-EALREADY` is reported, not gated, but it is now reached.** One to three
rounds per thousand, which is too few to gate on and quite enough to test with.
That is why the `CHECKSUM` cancel loop is a thousand rounds while the others are
three hundred: at three hundred it found the window in only two runs out of
three, and the whole point of the loop is to reach it.

Both halves of the refcount rule are consequently verified, which is new.
Skipping the drop the cancel owes on a true return leaks 788 objects and wedges
`rmmod` for the full retry loop, tripping the kmemleak gate and the
release-timing gate. Dropping it on a false return is an immediate
use-after-free that KASAN reports through the dmesg gate, in four runs out of
four — and in only two out of three before the loop was lengthened, in exactly
the runs that reached the window.

### What the short fuzz costs

Three fixed seconds instead of ten minutes: roughly 35,000 submissions instead
of eight million. Interleavings that need millions of rolls will not reproduce,
and that is a real loss with no compensation. `KORU_SEED` replays a failure but
there is deliberately no duration knob, because a knob invites a slow default.

Two things had to change to survive the shorter run. The coverage gate demanded
that every opcode *succeed* at least once; at three seconds `READ` succeeds
about a hundred times rather than two hundred, and at the original handle-table
size it succeeded twenty-six times, which would eventually flake. The gate is
now that every opcode *completes* at least once, with the per-opcode success
claim carried by the deterministic sections, which cover all seven of them.
And the handle table went to 128, which turned most of the `EMFILE` wall into
real opens: 254 successful `OPEN`s and 102 `READ`s in three seconds, against 72
and 26 before.

### What was dropped outright

Loading the `rust_minimal` sample, because `insmod koru.ko` twelve lines later
is a strictly stronger test of the same thing. The kmemleak scan taken while the
module is still loaded, keeping only the post-unload scan that was always the
authoritative one. And a stale placeholder asserting that an unimplemented
opcode is `-EINVAL`, which had pointed at `OPEN` since T9 implemented it.

### The shared ring

Most sections share one ring rather than building their own, and `ring_quiesce`
runs between them and asserts nothing was left in flight, so a section that
walks away from queued work fails at its own boundary instead of confusing the
next one. Its first version broke on exactly that: it stopped at the first empty
reap, which on a busy ring is just the timeout expiring, so it reported zero
while a delay was still running. It now needs three consecutive empty rounds,
which are free on an idle ring because `min_complete` 1 returns at once when
`inflight` is zero.

Sections whose geometry is itself the property under test still build their own:
the `SETUP` rejection matrix needs virgin descriptors because `SETUP` is
one-shot, slot exclusivity needs eighty slots to span two bitmap words, handle
exhaustion needs a table of eight, the mmap matrix needs a configured but
unmapped descriptor, and the credentials test maps after forking.

### `koru_abi.h` split out

The ABI mirror moved out of `test/koru_test.h` into `test/koru_abi.h` on its
own, so that T14 could replace it with `cpp/include/koru_abi.h` as an include
swap rather than surgery inside a header that also carries harness
declarations. That is what happened: there is no copy under `test/` any more,
`test/koru_test.h` includes the real mirror with angle brackets, and
`test/Makefile` carries `-I../cpp/include`.

### What was verified, and how

Four deliberate breakages, each rebuilt and run, because the consolidation
changes how the suite detects things rather than only how fast it runs:

- An unreachable 128-byte allocation per `OPEN`. The kmemleak gate fires. This
  is the one the whole time budget depends on.
- Skipping the reference drop `CANCEL` owes on a true return. 788 unreferenced
  objects, and `rmmod` wedged for the full retry loop: both gates fire.
- Dropping that reference on a false return instead. KASAN reports a
  use-after-free in `refcount_dec_and_test` and the dmesg gate fires.
- Holding the arena mutex across `kernel_read`. The binary itself passes every
  assertion and the dmesg scan fails the run on the circular-locking warning,
  which is precisely why that scan has to gate.
- `release` no longer cancelling queued work. The two-second `rmmod` assertion
  fires.

The third of those is the one worth noticing. It passed cleanly at first, and
the reason was not that the check was weak but that the bug is unreachable until
the race window is wide enough to enter. A green run against a deliberately
broken kernel is a statement about coverage, not about correctness.

## Licensing

`kernel/` is GPL-2.0 and stays that way: the module uses GPL-only symbols and
declares `MODULE_LICENSE("GPL")`, so it could not load otherwise. Everything
else is MIT — the bindings, `test/` and `scripts/` — which is what keeps a
program written against koru from being obliged to be GPL by the binding it
links. That obligation is the thing a userspace API most needs to avoid, so a
new file under those directories takes MIT, never GPL by reflex.

`kernel/koru_abi.rs` is GPL-2.0 as part of the module while its userspace
mirrors are MIT. They are the same author's work, which is what makes the split
available to make; it is not a relicensing of somebody else's code.

## The Rust binding

`rust/sys` is the raw layer: the ABI mirror, the ioctl wrappers, `Ring`,
`Arena`, `BufPool` and the errno table. The kernel side is unchanged.

### Two toolchains became one, at 1.98.1

T13 began with Debian's rustc 1.95.0 and no cargo at all. Cargo arrived through
rustup, which also brought rustc 1.98.1 and put `~/.cargo/bin` ahead of
`/usr/bin`; the Debian packages were then removed. That broke module builds
outright, because the dev kernel was configured against 1.95.0 and kernel Rust
needs `rust-src`, which rustup had not installed.

The whole project moved to 1.98.1: `rustup component add rust-src`,
`olddefconfig`, a full kernel rebuild. The tree accepts it: the floor is
1.85.0 and `init/Kconfig` already gates a feature at `RUSTC_VERSION >= 109800`.
The only config change was `RUSTC_CLANG_LLVM_COMPATIBLE` going away, because
rustc now carries LLVM 22 against clang's 21. It gates nothing but
`RUST_INLINE_HELPERS`, an EXPERT cross-language LTO option that was never on.
Every option in
`scripts/koru-debug.config` survived, checked mechanically rather than by eye.

### Zero dependencies, and why it is worth keeping

`koru-sys` has no dependencies at all. `std` already covers opening files,
reading `/proc`, both clocks, sleeping, pipes, `strerror` and errno; only
fourteen symbols have to be declared by hand in `src/sys.rs`, each with its C
signature quoted above it. Nothing checks those prototypes, which is the same
written obligation the kernel side carries for `filp_open`.

rustup's cargo has no distribution crate cache, so a single dependency would
mean a network fetch and a lockfile pinned to whatever `stable` was that week.
An empty `Cargo.lock` is the artifact that proves the claim.

Two simplifications fell out. `getpwnam` is replaced by parsing `/etc/passwd`,
which keeps the forked credentials child free of allocating libc calls. And no
threads: `koru_race.c` links `-pthread` but creates none, and a thread would
make every later `fork` unsafe.

### The ioctl number is derived on both sides

`src/sys.rs` reimplements the asm-generic `_IOC` encoding, so the three command
numbers come from `size_of::<KoruParams>()` and `size_of::<KoruEnter>()` exactly
as the kernel's `kernel::ioctl::_IOWR::<T>` does. A struct size change therefore
moves the number on both sides at once, and the kernel's size-and-direction
check rejects a skewed userspace with `EPROTO`.

That was verified by adding a field to `KoruParams` on the kernel side alone:
the host unit tests still pass, and 56 of 58 device tests fail with errno 71 at
`SETUP`. The three literal constants pinned in a unit test are a **canary for
that break**, not a duplicate of the arithmetic. Without them it surfaces only
as an unexplained `EPROTO` inside the VM. Do not delete them as redundant.

### What is safe, and what cannot be

`Ring::enter` is safe, and the reason is the strongest thing the Rust binding
has to say: `sq_addr` and `cq_addr` are derived from real slices, so the kernel
reads and writes memory Rust has proved is live, and `cq: &mut [Cqe]` proves
exclusivity for the half the kernel writes. It takes `&self`, because the kernel
serialises submitters itself and T15 will hold the ring behind an `Rc`.

`Arena::slot` and `slot_mut` are `unsafe`. A kworker writes into the arena, and
handing out `&mut [u8]` over memory a kernel thread is concurrently writing is
UB by Rust's own rules whatever `MAP_SHARED` says. `BufSlot` is the wrapper that
discharges the obligation: it is move-only, and submitting an op *moves* it into
the op state, so no `&mut` can exist while the kernel holds the slot.

**`Arena` deliberately does not borrow `Ring`.** The mapping outlives
`close(fd)`, and the test for that is `let a = ring.mmap()?; drop(ring);
a.slot(2)`. A lifetime tying the two together would make that property
inexpressible rather than merely untested.

**The raw layer stays `pub`.** Most of the rejection matrix cannot be expressed
through the safe API: a well-typed `enter` cannot send `to_submit` past
`sq_entries`, a non-zero reserved word, an unmapped `sq_addr`, or a `SETUP`
encoded read-only. Hiding `enter_raw`, `setup_raw` and `ioctl_raw` would make
the safe API's claim to be safe untestable. This is a deliberate export.

### `enter` must lose none of its three outcomes

The ioctl carries a consumed count, a `submitted`/`completed` writeback, and an
errno, and the writeback happens **even on `-EINTR`**. So `EnterError` carries
`Progress` alongside the errno, and `enter_raw` reads the writeback out of the
struct *before* it looks at the return value. Breaking that order was tested
from both ends: deleting the kernel's writeback fails only the SIGINT test,
and reordering the two reads in the binding fails it too.

A short `submitted` or `completed` is never an error. Admission control produces
the first, a timeout or quiescence the second.

Two smaller traps. `Some(Duration::ZERO)` would map to `timeout_ns = 0`, which
means *no cap*, so it is rejected locally rather than silently inverting the
caller's intent. And the suite asserts `consumed == progress.submitted` on every
success — E1 reports the two independently, so checking they agree is free and
the C suite never did it.

### The errno table

`KORU_ERRNOS` is a closed set of every errno koru can produce, each row carrying
the kernel path it comes from. The reason column is what makes the table
reviewable instead of a copy of `errno.h`.

Five rows are judgement calls, settled here rather than rediscovered later.
`EBUSY` is a slot collision and `EALREADY` a cancel that lost its race, so both
are `Again`. `EMFILE` is an exhausted handle table, a resource limit, so
`NoMemory`. `EPROTO` and `ENOTTY` are version skew, so `Unsupported`.

`Error` carries the vocabulary name *and* the raw errno, which is what keeps the
mapping lossless where several errnos share a name. `Kind::Closed` has no errno
preimage at all: end of file is `res == 0`, and T30's `read_chunk` is what turns
one into the other. T20 adds the aliases and `?` conversions over this table
unchanged, and should re-export `Error` rather than define a second type.

### Idiomatic `#[test]`, and the two gates it needs

The suite is ordinary `#[test]` functions rather than a transcription of
`koru_check.c`'s section table. The C harness's soft assertions are genuinely
better, since a section reports every failure it has rather than only the
first, so tests are cut finely enough that one failure hides little. It runs
with
`--test-threads=1`, because three tests fork and several read process-global
counters.

That choice opens two ways to pass without proving anything, and both needed a
gate in `scripts/rust.sh`:

- **A filter matching nothing exits 0.** `cargo test -p koru-sys nosuchname`
  runs zero tests and succeeds. The script runs unfiltered and asserts the
  passed count against `WANT_PASSED`. Raise it when a test is added.
- **A skip is indistinguishable from a pass.** Preconditions that fail print a
  `KORU-RS-SKIP` marker and the script fails the run on it. That makes the Rust
  suite stricter than the C one, where `sec_creds` can skip and still pass;
  worth back-porting.

The skip gate had a real bug when first written: it anchored the grep at
`^KORU-RS-SKIP`, but under `--nocapture` libtest prefixes the line with
`test <name> ... `, so it never matched. Found by forcing a skip and watching
the run pass. A gate that has not been shown to fire is not a gate.

Three shapes the `#[test]` choice forces. A forked child may call only the raw
prototypes and must leave through `_exit`: a panic would unwind into libtest and
report twice, and a `println!` would flush the parent's inherited buffer. The
credentials test maps the arena *after* forking, because `VM_DONTCOPY` means the
child cannot inherit it. And a test that can hang arms `alarm()` itself, with a
handler that `_exit`s — never a watchdog thread, which would make every later
fork unsafe.

### Its own boot, and a flat leak window

`scripts/rust.sh` and `scripts/run-rust.sh` mirror `check.sh` and `run.sh`:
insmod, run, rmmod, kmemleak, taint exactly 4096, and the same gating dmesg
scan. `check.sh` is untouched, because it carries every pass condition for the
kernel and is meant to stay small.

One deliberate difference. `check.sh` earns its leak window by running bulk
allocators first and stamping `KORU-HEAVY-END-MS`, and that ordering is
load-bearing. libtest orders tests by name, so this suite cannot promise it;
`rust.sh` pays a flat six seconds instead. The kernel is unchanged at T13, so
leak coverage belongs to `check.sh` and the Rust scan is a regression net.

### A race that was nearly a flake

The slot-exclusivity tests first used 4 KB slots, copying the C section's
geometry. Two `CHECKSUM`s on one slot in a single batch are a collision only
if the first op's worker cannot finish before the submit loop dispatches the
second, and at 4 KB it sometimes could. The test failed as collateral damage
during an unrelated breakage, then passed eight times out of eight in
isolation. Raised to 64 KB, the same reasoning this file already records for
the shared ring, it became deterministic.

Worth generalising: a test that asserts a race was won is only as good as the
window, and the window is a property of the geometry, not of the assertion.

### What the suite does not re-express

`devchurn`, because `ringchurn`'s 300 lifecycles already cover open and close.
The hostile-userspace fuzz, because its target is the kernel's validation
surface, which the C fuzzer covers, and a second transcription of its per-opcode
oracle adds no information. And the two `rmmod` races, which need a second
process driven by the shell against an unchanged kernel and are already gated by
`check.sh`. The `setup` and `ioctl` matrices *are* included despite being T3,
because T13 owns the ioctl numbers and a wrong direction bit is the likeliest
defect in a hand-written `_IOWR`.

### What was shown to fail

Every one of these was applied to the kernel, rebuilt, run, and reverted. The
value is in which tests failed, not that some did.

- Neutralise the ioctl **direction** check, or return `EPROTO` where the
  dispatcher owes `ENOTTY`: only `ioctl_dispatch_matrix` fails, once each.
- Return the **completion count** from `ENTER` instead of the consumed count:
  14 tests fail, including the new `consumed == submitted` cross-check.
- Delete the **`EINTR` writeback**: only the SIGINT test fails. Reordering the
  two reads in `enter_raw` fails it from the userspace side too.
- Make `slot_try_acquire` **always succeed**: the four exclusivity assertions
  fail and every distinct-slot case stays green.
- Delete the **slot bounds guard** before `slot_try_acquire`: the guest dies and
  the run reports no verdict, which is the gate firing the hard way.
- Stop bumping the **handle generation** on `CLOSE`: only the two handle tests
  fail.
- Accept **`MAP_PRIVATE`**: only `mmap_rejection_matrix` fails.
- Add a field to **`KoruParams`** kernel-side only: the host unit tests still
  pass, and 56 of 58 device tests fail with `EPROTO` at `SETUP`.

The cancel-versus-running-`CHECKSUM` loop reaches the `-EALREADY` window about
once per thousand rounds, matching what the C suite measures. That is what makes
the cancel refcount rule testable in both directions, so the count is printed
rather than gated.

## The ABI conformance diff

T14 turned the agreement between the two userspace mirrors into a `diff`. Each
side has an `abi_dump` that prints one record per line — constants, ioctl
numbers, opcodes, every struct with every field, evaluated handle-encoding
vectors, the fifteen vocabulary names and the twenty-nine errnos — and
`scripts/abi.sh` compares them. It needs no VM, no device and no module, which
makes it the fastest gate in the project.

Four decisions inside that are not obvious.

**Every number is decimal, ioctl request numbers included.** A hex format that
differs between the two emitters fails the diff while carrying no ABI content
at all. The hex forms stay where they are read by people: the header's own
`static_assert`s and the canary in `sys.rs`.

**Each field record carries a spelled-out type name.** Offsets and sizes cannot
see `Cqe::res` turning from `i64` into `u64`, because nothing about the layout
changes. The type column is hand-written on both sides, which is the mirror
duty made visible.

**A record dropped from both emitters at once diffs clean**, and that is the
one failure the comparison cannot see by itself. So each struct's record
declares its field count, and each emitter asserts that its field sizes sum to
`sizeof`. All four structs are padding-free, so a field forgotten on both sides
aborts the emitter rather than passing quietly.

**A bare `diff <(a) <(b)` is not a gate.** It discards both exit statuses, so a
C++ binary that dies before printing and a cargo build that fails compare two
empty streams and report success — the same hole `WANT_PASSED` closes in
`rust.sh`. `scripts/abi.sh` checks both exit statuses, both lengths and a
`WANT_RECORDS` minimum before it diffs anything, and doc/Plan.md's verification
step names the script rather than the one-liner it used to.

The errno table's prose `produced_by` column is deliberately **not** dumped. It
is provenance documentation rather than wire format, and diffing it would make
a wording improvement a two-language edit while proving nothing; a miscopied
row still shows up as a wrong errno or a wrong vocabulary name. The C++ side
also had to carry the whole table at T14 rather than at T39, because the diff
cannot be empty otherwise. T39 consumes `cpp/include/koru_errno.h`; it does not
write it.

### The limit, stated plainly

**The diff compares two userspace mirrors and cannot see the kernel.**
`kernel/koru_abi.rs` is canonical and is in neither dump. The only thing tying
either mirror to it is `setup_get_params_works_before_setup`, which now asserts
that every cap `GET_PARAMS` reports equals its `KORU_MAX_*` constant exactly,
where it previously asserted only that they were non-zero, and that
`handle_count` 0 becomes `KORU_DEFAULT_HANDLES`. That runs in the VM, not on
the host.

### What was verified, and how

Six perturbations, each reverted, each proving a different mechanism.

- `KORU_MAX_SLOT_COUNT` 4096 to 4095 in the C header. Compiles clean and
  `scripts/abi.sh` fails, which is the case no `static_assert` can reach: a
  constant is not a layout.
- A `pad` field inserted before `koru_sqe.handle`. Fails at **compile time**,
  in both the C++ build and `make -C test`, ten static assertions at once. A
  layout fault therefore surfaces earlier than a constant fault, and never
  reaches the diff.
- `KORU_DEFAULT_HANDLES` 64 to 32 in `abi.rs`. The gate fails, so the diff has
  teeth in the Rust direction and not only the C one.
- `ECANCELED`'s kind from `Cancelled` to `Io` in `error.rs`. The gate fails, so
  the errno half is genuinely compared rather than merely printed.
- `KORU_MAX_HANDLES` 4096 to 2048 in the **kernel** file, rebuilt and run in
  the VM. The strengthened cap assertion fails and `scripts/abi.sh` still
  passes. That negative result is the point: it is what the limit above looks
  like from the outside.
- An `abi_dump` whose `main` returns 0 without printing. The gate fails on the
  record count, which the process-substitution one-liner would not have done.

One thing broke on the way, and it is worth not rediscovering. Adding a
`[[bin]]` to `koru-sys` made `cargo test --test kernel --message-format=json`
emit **two** executables, and `run-rust.sh` took the last one, which is
`abi_dump`. It ran no tests, exited 0, and only the `WANT_PASSED` floor caught
it — exactly the failure that gate exists for. Selecting on `"test":true` is
not the fix either, because a bin target carries that field too; the runner now
selects on the target kind.

## Futures and the executor

T15 built `rust/runtime`: a slab of op state keyed by a generational cookie, a
future per opcode, and a single-threaded executor whose park is
`ENTER(min_complete = 1)`. The ownership rule the design section states —
the slab entry owns the `BufSlot`, the future owns only the cookie, a drop
frees nothing — survived contact unchanged. What follows is what it did not
say.

The crate is `#![forbid(unsafe_code)]`. `koru-sys` carries all of it, and the
executor's wakers go through `std::task::Wake` on an `Arc` rather than a
hand-rolled `RawWaker` vtable over an `Rc`, which would be unsound the moment
a waker crossed a thread. T51's waiter thread is exactly that, written down in
advance, so the safe route costs a mutex lock per wake and buys a checkable
claim. Dependencies stay at zero, so the lockfile still lists nothing but the
workspace's own packages.

### `Queued` is not `Live`, and that distinction is the whole task

Futures are lazy in the Rust convention: nothing is registered and nothing is
queued until the first poll, so an unawaited future costs nothing and hands
its slot straight back. Lazy registration means an op has a state the design
section never named — sitting in our own batch, with the kernel unaware of it.

Dropping one of those must drop its SQE and must **not** submit a `CANCEL`.
`cancel_op` looks its target up in `RingCtx::pending`, would answer `-ENOENT`,
and no completion for the target would ever arrive; the entry and its slot
would then leak for the life of the process. Leaving the SQE in the batch is
worse: it submits an op naming a slot that is already back in the free pool,
and the next user of that index gets `-EBUSY` from the `slot_busy` bitmap,
which is verbatim the failure T16 exists to catch.

So the states are `Queued`, `Live`, `Ready`, `Abandoned`, `Dead` and
`CancelProbe`, and three of them are drop cases rather than one. `Ready` is
not a corner: `OPEN`, `CLOSE` and `NOP` complete inline inside the submitting
`ENTER`, so a future is very often already complete by the time anything drops
it, and a `CANCEL` there is a wasted SQE that always answers `-ENOENT`.

The `Queued` to `Live` transition is per **consumed prefix**, never per batch.
A short `submitted` is admission control, so only `batch[..submitted]` becomes
`Live` and the tail goes back on the pending queue still `Queued`. Marking the
whole batch instead double-counts in-flight ops, resubmits SQEs the kernel has
already consumed, and hangs the run.

`CancelProbe` exists because a `CANCEL` is an op like any other: C1 gives it
its own completion, and it needs a unique cookie of its own, since a duplicate
`user_data` cancels an unspecified one of the two.

### `to_submit` past `sq_entries` fails the ioctl

Admission control produces a short count, but `to_submit > sq_entries` is a
protocol failure and fails `ENTER` outright, as does `cq_space > cq_entries`.
A batching executor therefore has to chunk its own batch to the queue depth;
discovering this cost one VM run with `ENTER failed: EINVAL`. It is not an
admission-control interaction and no amount of retrying helps.

### Three cells, one order

`RefCell<Slab<Task>> > RefCell<Slab<Op>> > RefCell<Vec<Sqe>>`, and no borrow is
held across a poll, across a wake, or across the drop of a payload. Every
borrow in the reactor is scoped to a block that *decides*; the acting happens
after it is released. Concretely: dispatch takes the payload out and ends the
borrow before dropping it, `Future::drop` releases the slab before pushing its
`CANCEL`, and the executor takes the boxed future out of its task slot before
polling it — otherwise the first poll that spawns or wakes panics with
`already borrowed`. The ready queue is drained into a local before any poll for
the same reason: a self-waking task would deadlock on a non-reentrant mutex.

`Ring::enter` and `BufPool::acquire` both take `&self`, so neither the ring nor
the pool needs a cell. Do not add one.

### The empty-ring foot-gun, on this side

The kernel returns rather than sleeps whenever `min_complete` is unreachable,
which is precisely when nothing is in flight. So the userspace failure is not a
D-state hang but a spin: an executor that parks with every task pending,
nothing queued and nothing in flight burns a core forever. That state is a bug
in the program, so it panics with a message naming it. The predicate is a pure
function of three counts, which is what makes its whole truth table testable
with no device.

"Poll, do not block" is `min_complete = 0`, never a zero timeout: `timeout_ns`
0 means *no cap*, and `koru-sys` already rejects `Some(Duration::ZERO)` for
that reason.

### Two shapes the plan's sketch got approximately right

The plan's demo writes `let (n, buf) = read(h, buf).await?`. The real output is
`BufResult<T> = (Result<T, Error>, BufSlot)`: the caller must get the slot back
on the error path too, or every failed read leaks one slot of `slot_count` for
good. That is forced by completion-based I/O, not a style choice.

It also writes `open("/etc/hostname")` with no buffer. Every open needs a slot
for the path, and the raw layer takes it as an argument rather than reaching
into the pool, so T30 owns every acquisition — which is what T30's own
description already says. The consequence is that T15 needs no waiter queue for
slot exhaustion; T30 does.

Ops are methods on the runtime handle, named after the opcodes, so the
free-function namespace stays clear for T21's ambient surface and T30's Braam
names. `Error` is `koru_sys::error::Error` unchanged, so T20 re-exports rather
than introducing a second type, and end of file stays `res == 0`.

### `Stats`, because none of this is otherwise observable

`enters`, `sqes_submitted`, `cqes_reaped`, `cancels_submitted`, `eintrs`, and
the three gauges. T16's "assert a `CANCEL` is submitted" and "the slot is not
back in the free pool until the target's CQE lands" cannot be written without
it, and T31's measured `ENTER` count needs the same counter. It also makes the
inline-completion claim assertable: a lone `OPEN` costs exactly one `ENTER`.

Teardown reaps until nothing is in flight, bounded, so the free count means
something at exit. Without it the ordinary case of dropping a future in the
last poll before `block_on` returns leaves an entry whose completion never
arrives. It is not a safety bug — closing the fd makes the kernel cancel
everything — but it makes the accounting untestable.

### A forked libtest child that panics exits 0

The EINTR test forks, because libtest runs each test on a spawned thread and a
process-directed alarm lands on the main one rather than the parked one. The
first version then passed against a deliberately broken executor. The child
panicked exactly as intended, printed the panic, and exited **0**: it is a fork
of a worker thread, so there is no harness left to report the failure to, and
the process ends cleanly when that one thread unwinds. The parent's verdict was
vacuous.

The child now wraps its body in `catch_unwind` and leaves through `_exit` with
a distinct code. Notes already said a forked child must leave through `_exit`;
the reason turns out to be stronger than double-reporting.

### The two suites share one boot

`scripts/rust.sh` grew a `run_suite` function and the runner an explicit suite
list, so `koru-sys`'s `kernel` and `koru`'s `runtime` run in one VM boot under
one verdict. The floor stays **per suite**: a single total would let one
crate's growth mask a filter typo that ran none of another's, which is the hole
`WANT_PASSED` exists to plug. Adding a test raises that suite's number in
`scripts/run-rust.sh`.

### Eager or lazy: the bindings will differ, on purpose

The C++ awaiter sketch below has `await_ready() -> false // op already
submitted`, which is eager. An eager binding has no `Queued` state and its
destructor always submits a `CANCEL`. Both choices are sound; write down which
one each binding made, or T40 will transcribe a state it does not need or drop
a filter it does.

### What was verified, and how

Six perturbations, each reverted.

- Delete the generation comparison in the slab. Two host tests fail, the
  recycled-index one and the cookie-zero one, and nothing else. Both genuinely
  depend on it.
- Stop filtering dead SQEs out of the batch. The queued-drop test fails, and
  earlier than expected: the state machine sees a completion for a `Dead` entry
  and refuses it before the kernel's `-EBUSY` can appear.
- Return the slot to the pool in `Future::drop` instead of leaving it in the
  entry. The live-drop test fails on the free count, which is T16's assertion
  reached early.
- Mark the whole batch `Live` rather than the consumed prefix. The run hangs
  and the runner reports no verdict, which is what a hang looks like from
  outside.
- Treat `EINTR` as a hard error. The forked child panics and the parent's
  verdict fails — but only after the `catch_unwind` fix above, which is how
  that fix was found.
- Remove the stall predicate. The executor spins, the test's own alarm fires,
  and the binary exits 99.

The demo needs no perturbation to have teeth: the timers are armed 30, 10, 20
and asserted to resolve 10, 20, 30, so an executor that ran them serially would
record submission order and fail.

## Drop safety under a race

T16 added `race`, the crate's first combinator, and one integration test that
drops a `READ` future mid-flight tens of thousands of times. T15's drop tests
stage the state they want with `PollOnce` and a forcing `nop`; this one lets
the kernel decide, which is what turns an asserted mechanism into a falsified
one. Nothing in the state machine changed: every perturbation below broke
something that was already there.

### What "under ASan" means here, decided rather than discovered

Rust's AddressSanitizer is `-Zsanitizer=address` on nightly and the project is
pinned to stable 1.98.1. **No nightly toolchain was added.** The `koru` crate is
`#![forbid(unsafe_code)]`, so there is no Rust-side undefined behaviour for ASan
to find; every unsafe block in the userspace stack is in `koru-sys`, which T13
covered. The substitute is the one the plan named: the kernel's KASAN, the
dmesg gate `scripts/rust.sh` already applies to the whole run, and the pool,
slab and inflight accounting `Stats` exposes. The accounting half is asserted
as `cqes_reaped == sqes_submitted` with `inflight` and `slab_live` at zero,
which is C1 end to end — an op whose completion never arrived breaks it.

This is a statement about Rust only. T43 is the C++ mirror of this task and
keeps its real ASan requirement, because a dangling coroutine frame is a
genuine use-after-free there.

### `race` drops its loser, and that is the whole mechanism

`race` is an `async fn` holding both futures in `pin!` locals and polling them
through `poll_fn`. No `Unpin` bound, no allocation, no unsafe: it takes async
blocks as readily as the op futures. When it returns, the frame drops and the
loser with it, which runs `abandon_on_drop!` and hence `Inner::abandon`.

Two properties that are not decoration. Both sides are polled before it
suspends, or one starves. And **which side is polled first alternates**: with a
fixed order, a race between two futures that are both already `Ready` always
resolves the same way, and the test's own numbers show what that costs —
pinning the order sent the "read was already complete" bucket from 0 to 2,905
of 4,000 and the read-won bucket from 97% to 25%.

The drop lands inside the caller's poll with no slab borrow held, because
`poll_task` takes the future out of its slot first. `abandon` then takes `slab`
and `pending` in that order, which is the existing lock order. The combinator
introduced no new one.

### A timer cannot fire inside a read, which shapes the whole test

`delay_jiffies` rounds a delay **up to whole milliseconds** and a whole-slot
64 KB read takes about 120 µs on the dev VM. So every sub-millisecond delay
waits a full jiffy and the read always wins: a nanosecond-granularity sweep
straddles nothing. The first version of the test swept 0 to twice the measured
read latency and recorded 400 read wins out of 400.

Only `ns == 0` races, because `enqueue_delayed` with zero jiffies queues the
work immediately. Two further findings followed:

- **Submission order decides which kworker item runs first.** With
  `race(read, delay)` the read's SQE is first in the batch, so its work item is
  queued first and the timer runs behind it; both completions then land in one
  `ENTER` and the read is found `Ready`, never in flight. Putting the timer
  first — `race(delay, read)` — sends that bucket to zero and makes the
  in-flight drop reachable at all.
- **The in-flight arm is rare by an order of magnitude**, about 2.8%: 2,650 of
  100,000. That is not a defect in the test, it is the shape of the race, and
  it is the same lopsidedness the kernel suite's cancel races already assert
  floors against. Background load on the workqueue was tried and rejected: it
  moved a 120 µs read to 150 µs against a 1 ms floor, which is not a gap load
  can close.

The floors are therefore set well under the measured rates, not near them, and
the sweep spends three rounds in four at `ns == 0`.

### What the race test asserts, per round

The slot under test is reacquired after every round, and the free list is LIFO,
so it is the same index. The three buckets are told apart by the
`cancels_submitted` delta alone.

When the read was live, its slot must **not** be in the free pool, the entry
holds it until the target's own CQE lands, and the op that follows on that
index must not get `-EBUSY`. **That errno is asserted by name**, because it is
the only symptom a prematurely freed slot ever produces, and a bare `is_ok`
would report it as something else entirely.

### Shown to fail

Five perturbations, each reverted.

- Return the slot to the pool in `abandon`'s `Cancel` arm. Fails on the free
  count first, which is T15's assertion; with that one relaxed it reaches the
  probe and fails with `EBUSY(16)` by name. Both halves were run, because the
  `-EBUSY` claim is the one T16 adds and a perturbation caught earlier proves
  nothing about it.
- Make the cancel name the probe as its own target rather than the read. Fails
  straight through to the `-EBUSY` probe with no assertion relaxed, which is
  the cleanest demonstration the probe has teeth.
- Make the op future's `Drop` abandon nothing. No `CANCEL` is submitted, so the
  round is classified as already-complete and fails on the free count there.
- Stop alternating which side `race` polls first. Caught twice: the host unit
  test for alternation, and the integration test's read-won floor.
- **Drop the `CANCEL` push entirely, and T16's test still passes.** A read
  completes on its own in about 120 µs, so the slot comes back regardless; the
  cancel is not load-bearing when the target self-completes. What catches it is
  T15's `a_cancelled_op_is_dequeued_rather_than_waited_out`, whose target is an
  hour-long delay, and it catches it as a hang. Worth recording rather than
  quietly counting as a sixth success: the two tests cover different halves of
  the same rule, and neither subsumes the other.

### Cost

The full 100,000 rounds take 14 seconds of the guest's time and pass. The
everyday gate runs 4,000, which is under a second and still yields about a
hundred in-flight drops. `KORU_ITERS` raises the count, `KORU_SEED` replays a
jitter, both forwarded into the guest by `scripts/run-rust.sh`, and each test
prints the pair it used. The full run needs a `TIMEOUT` past the runner's
600-second default.

## Braam's vocabulary in Rust

T20 is the first piece of the Braam surface, and almost all of it already
existed: `Error`, `Kind` and the errno table were written at T13 because the
ABI dump needed them. So the task was mostly deciding where things live and
what the fifteen names are allowed to cost.

### The re-export, and where the orphan rule puts the conversions

`koru::vocab` re-exports `koru-sys`'s `Error`, `Kind` and `Errno` and adds the
`Result` alias and the type aliases. It defines no type of its own, because a
second `Error` would be visible at every T15 call site at once — every op
future's `Output` carries one.

The `?` conversions could not follow it there. `io::Error` and `Error` are both
foreign to the `koru` crate, so `impl From<io::Error> for Error` is an orphan
impl and does not compile; the impls live in `koru-sys` beside the type and the
`koru` crate re-exports the type that carries them. Worth stating plainly
because the instinct is to put the whole surface in the surface crate, and the
compiler only objects once you have written it.

Four conversions, which together are what "native `?`" means here: from
`Errno`, from `io::Error` for the ordinary POSIX calls the binding still makes,
from `EnterError` for the ioctl, and back into `io::Error` so a koru error can
leave for any Rust caller. The `EnterError` one drops the writeback, which is
fine only because a caller who propagates with `?` has stopped caring what was
consumed; keep the `EnterError` itself to resubmit.

### Braam's `Error::Cancelled` is `Kind::Cancelled` here

Braam's `Error` is a bare enum, so `r.error() == Error::Cancelled` is ordinary
equality. Ours carries the raw errno as well, and that equality has to keep
`EBUSY` and `EALREADY` apart even though both are named `Again`. So `==`
between two `Error`s compares both fields, and the Braam idiom is served by
`PartialEq<Kind>` — `err == Kind::Cancelled` — plus `err.is(Kind::Cancelled)`
where a method reads better. This is one of the few places the Rust surface
cannot be a transcription: C++ gets the name back at T44, where `Error` can be
the enum and the raw errno can ride alongside in `result<T>`.

### Exactly one name is synthesised, and it has raw errno 0

`Kind::Closed` has no errno preimage: end of file is `res == 0`. That makes it
the one vocabulary name userspace has to invent, so `Error::closed()` is the
only constructor that does not start from an errno, and it sets raw 0 — a value
no kernel path can produce, so "raw 0" reads unambiguously as "made up here".

Resisting the general `Error::from_kind(kind)` is deliberate. The errno is the
lossless direction; a name invented in userspace cannot go back to one, and a
constructor that let any of the fifteen be built without an errno would make
`raw()` a lie wherever anyone used it. There is one name that needs it and one
constructor that provides it.

The same 0 is what an `io::Error` with no errno at all becomes — `Io`, raw 0 —
and `Display` falls back to Braam's own wording there. Everywhere else
`Display` stays the OS message, because this binding is Linux's and `ENOENT`
says more than `not found`.

### Three aliases, and the macros that are not needed

Braam's `String`, `Option` and its fixed-width integer names are Rust's own
spelling already, so the alias list is `Str`, `Span` and `SpanMut`. Braam's
`Span<T>` is the mutable one and `Span<const T>` the shared one, because its
const-ness sits in the element type; in Rust it sits in the reference, so the
unsuffixed name is the shared one and the mutable one gets the suffix.

`TRY`, `TRY_VOID`, `CO_TRY` and `CO_TRY_VOID` have no Rust counterpart to
write: `?` is all four, inside an `async` block as well as outside one. That is
the one place the Rust surface is strictly shorter than Braam's, and T44 has to
write the macros because C++ has no such operator.

### What the done test proves, and what it cannot

The table-driven check walks `KORU_ERRNOS` and asserts, for every row, that the
errno maps to one name, that the `Error` still carries the raw value, that the
bare-name comparison agrees, and that no errno appears twice. A second test
subtracts the mapped names from `KINDS` and requires the remainder to be
exactly `[Closed]`, so a sixteenth name added without an errno row fails
immediately. A third round-trips every row out through `io::Error` and back.

Six perturbations, each applied and reverted:

- `kind_of` always answering `Io`: the done test fails on the first row.
- `from_errno` discarding the raw errno: the done test fails on the raw half
  and the `io::Error` round-trip fails independently.
- `Kind::Closed` removed from `KINDS`: only the orphan-name test fails, and
  `scripts/abi.sh` fails too, on its record-count floor.
- `impl From<io::Error> for Error` deleted: the `?` test fails to *compile*,
  which is the only way a conversion test can have teeth.
- A row duplicated: the "exactly one" assertion fails.
- **A row's judgement flipped — `EACCES` from `Perm` to `Io` — and every test
  passes.** Both sides of the assertion read the same table, so the check
  proves the mapping is total and lossless, not that any row is *right*.
  `scripts/abi.sh` catches that one, by diffing the row against the C mirror,
  and the reason column in the table is what makes it reviewable. Two
  instruments, neither redundant; recorded because the first one looks like it
  should cover both.

`KINDS` also replaced `abi_dump`'s hand-written list of the fifteen, so the
emitter and the vocabulary cannot drift apart. The dump is byte-identical
across that change: 136 records, still agreeing with the C mirror.

## The ambient ring and the runtime entry

T21 is the first task whose deliverable is a *program* rather than a library:
Braam's hello world, running through koru. Most of it was straightforward and
three parts were not — the standard streams, the file position, and what a
program's exit means for what it spawned.

### A thread-local ring, and an attribute macro with no `syn`

`koru::rt` holds an `Option<Ambient>` in a thread-local: the `Runtime`, the
three standard handles, and the position table. `install` replaces it, which is
what lets a test have its own; `current` clones, which is cheap because a
`Runtime` is a handle and not the ring. Every free function — `block_on`,
`spawn`, `write_all` — finds it there, and that is the whole reason a Braam
program never names an executor.

`#[koru::main]` is a third crate, `koru-macros`, because a proc macro cannot
live in the library it serves. It has **no dependencies**: no `syn`, no
`quote`. The workspace lockfile still lists nothing but its own packages, which
is the property worth keeping — this is one signature, and parsing it is token
surgery. It finds `fn`, insists the token before it is `async` and the one
after it is `main`, renames that one token, and appends a real `main` that
calls the entry. Renaming one token rather than re-emitting the item is what
keeps the body's spans, so a compile error inside the program still points at
the program's own line. Accepting only `main` and only `async` is not
pedantry: both misuses produce a clear message instead of a broken expansion,
and both were checked by writing them.

### The standard streams, and T18's gate inherited

T19 recorded that a program whose stdout is a pipe or a tty cannot write
through koru unless whoever created that descriptor made it non-blocking, and
that this would be T21's problem. It is, and the answer is to re-open rather
than to modify.

`fcntl(F_SETFL, O_NONBLOCK)` on the inherited descriptor would work and is
wrong: descriptor 1 is usually a *shared* file description, so the flag reaches
the shell and every other process holding it — the classic way to break a
terminal. Opening `/proc/self/fd/1` instead gives a file description of our
own, and the flag stays inside the program. The whole probe is safe `std`,
which matters because the crate is `#![forbid(unsafe_code)]`: `metadata` of
`/proc/self/fd/N` follows the link and answers whether it is a regular file,
`/proc/self/fdinfo/N` carries the open flags in octal, and `OpenOptionsExt`
passes `O_NONBLOCK`. No `fstat`, no `fcntl`, no `unsafe`.

So each of the three is adopted directly when it is a regular file or already
non-blocking, and re-opened first otherwise. `ADOPT_FD` takes a reference of
its own, so the re-opened descriptor is closed on the way out.

**The re-open does not always work, and the VM is where it does not.** Under
`vng --exec` descriptor 1 is a virtio-serial port, and virtio_console permits
one open per port, so `/proc/self/fd/1` answers `-EBUSY`. On an ordinary
terminal, where it is a `/dev/pts` device, it succeeds. When it fails the raw
descriptor is adopted anyway and the write is refused, which is honest and
visible rather than silent. The consequence, stated plainly: **a koru program
run straight onto the VM console prints nothing; piped or redirected it works.**
That is why the gate runs each example three ways.

A descriptor that cannot be adopted at all becomes `Handle(0)`, which is never
a valid handle, so the kernel answers `EBADF` rather than the runtime guessing.

### The file position is userspace's, and hello world is what proves it

`READ` and `WRITE` carry an explicit file offset and never touch `f_pos`. The
plan calls the consequence `seek_fd` and files it under T30, as "pure userspace
bookkeeping" — but it is not deferrable to T30, because **Braam's hello world
writes three times**. With the offset stuck at 0 a redirected stdout ends up
holding `!\n` over the front of the greeting, which is exactly what the
perturbation produced.

So the ambient state carries a position per handle, and `write_all` advances
it. Two rules fall out. A handle is registered by *whoever creates it*, because
only the creator knows whether an offset means anything on it — `install` for
the three streams, T30's `open_at` for a path — so `register_handle` is public
API rather than an internal detail. And an unregistered handle counts as a
stream, offset 0, which is the only offset an unseekable file accepts anyway.

This is the first place koru's "no `f_pos`" decision costs userspace something
concrete. It is cheap, but it is not free, and it is now paid in one place.

### `EPIPE` joins the errno table

Braam's own `result.h` says it: `Closed` is "the far end of a stream is gone:
EOF to a reader, EPIPE to a writer". T20's table had no `EPIPE` row, because
until `WRITE` could reach a pipe nothing could produce one. It can now —
`prog | head` produces it on purpose — so the row is there, mapped to `Closed`,
in both mirrors.

That retires T20's "exactly one name has no errno preimage": `Closed` now
arrives both ways, from `EPIPE` and synthesised by `Error::closed()` for end of
file, and the test became "every name is reachable", which is the stronger
assertion. The socketpair test is what proves the row rather than asserting it:
drop the far end, write, and the error is `Closed` carrying raw `EPIPE`.

### What a program's exit means

Braam's rule is that the process ends when the **root** task returns, whatever
the others are doing, and `proc_spawn` is fire and forget. So the entry does
not drive spawned tasks to completion. `Runtime::drop_tasks` empties the task
slab; each dropped future's `Drop` abandons its op, which submits a `CANCEL`,
and the bounded `drain` reaps them. An hour-long delay in a spawned task must
not hold the exit, and when the perturbation made `shutdown` call `run()`
instead, the test did not fail — it **hung**, and the alarm killed the guest
process. A hang is a legitimate failure here as long as an alarm is armed.

The at-exit hooks are asynchronous, because the thing they exist for is
flushing a buffered writer and a destructor cannot await. They run last
registered first, like C's `atexit`, before the tasks are dropped.

Two interim shapes, both marked in the source. A non-blocking write that
answers `EAGAIN` has nothing to wait on until `POLL_ADD` arrives at T22, so
`write_all` backs off a millisecond through the ring rather than spinning. And
a program that finds the slot pool empty yields and retries; a proper waiter
queue belongs with T30, and with eight 64 KiB slots nothing reaches it.

### The examples are the entry's own test

`#[koru::main]` replaces `main`, so libtest cannot call one: the entry can only
be tested by running a program. `scripts/rust.sh` runs both examples in the
guest after the suites, and `scripts/run-rust.sh` asks cargo where each binary
is, for the same reason it does for the test binaries.

Each is run three ways, and each way is a different assertion. Through a
**pipe**, which is blocking and is therefore the re-open path. Redirected to a
**regular file**, which is the three-writes-at-three-offsets path. And with an
argument, which is `Args`. `read_file` writes the file to stdout and the timer
completion order to stderr, so "it printed the right bytes" and "the timers
resolved out of order" stay two separate assertions, and the second is what
makes it T15's demo rather than `cat`.

A filtered run skips the examples, because a filter is a test-name filter and
would otherwise run them unfiltered every time; an unfiltered run with no
examples at all fails, which is the same hole `WANT_PASSED` closes.

### What was shown to fail

Six perturbations, each applied and reverted.

- **The position always 0.** The runtime test fails on the file's contents, and
  the script's regular-file hello prints the three writes on top of each other.
- **The streams adopted as they are, with no re-open.** Every piped run
  produces nothing at all, and the redirected one still passes — which is the
  clearest statement of what the re-open is for.
- **`shutdown` running the spawned tasks instead of dropping them.** The test
  hangs rather than failing, and the alarm is what reports it.
- **The at-exit hooks in registration order.** Only the hook-order test fails.
- **The `EPIPE` row deleted.** The vocabulary's reachability test fails and
  `scripts/abi.sh` fails, one for each mirror.
- **`#[koru::main]` on a blocking `fn`, and on a function not called `main`.**
  Both are a compile error naming the reason, rather than an expansion that
  fails somewhere else.

## `OPEN` learns to create

T30 needed `copy_file`, `copy_file` needs to create its destination, and
nothing in the ring could create a regular file. `MKDIR` makes directories and
`SYMLINK` makes symlinks; `OPEN` had no `O_CREAT`, for the reason Notes gave
above and T26 already qualified: on that opcode `handle` is spent on the flags,
so there is no field for a creation mode.

The way out was there since T24. **Every path op carries its argument after its
path in the same slot**, at the first 8-aligned offset at or after the path's
end, and `OPEN` is where that placement came from. So the mode goes there too:
`KORU_O_CREAT` means "a `u64` mode follows the path", and without that bit
nothing past the path is read. An `OPEN` that creates nothing therefore needs no
room for an argument it does not carry, which is a real case — a path can
sit at the very end of a slot — and has its own test on both sides.

Four flags joined the whitelist: `KORU_O_CREAT`, `KORU_O_EXCL`, `KORU_O_TRUNC`
and `KORU_O_APPEND`. `O_APPEND` was already described by the ABI — `WRITE`'s
doc has said since T17 that an appending handle ignores `off` — and until now
was reachable only by adopting a descriptor somebody else opened that way.

Two rules are koru's rather than the VFS's. `KORU_O_EXCL` without
`KORU_O_CREAT` is `EINVAL`, where Linux tolerates it and does nothing: a flag
that cannot act is rejected, not dropped, which is the same rule as an unknown
flag bit. And the mode is masked to `KORU_OPEN_MODE_ALL`, which is `0o1777` —
`KORU_MKDIR_MODE_ALL`'s value for `KORU_MKDIR_MODE_ALL`'s reason. `S_ISUID` and
`S_ISGID` are refused rather than silently dropped, so koru creates nothing
setuid and a caller who asked for it is told.

The umask applies, and it is the *submitting* task's, because `filp_open` runs
inline in ioctl context. That is `OPEN`'s original creds argument paying for
itself a second time: in a kworker the mode would be masked by init's umask.

### The fuzzer's sandbox had a second hole

T26 found that the fuzzer's path sandbox was never real and rebuilt it. The
creation flags found the other half of the same problem, in the *hostile*
generator rather than the path one.

`gen_hostile` mutates one field of an SQE the valid generator just built. Case 7
sets a random `handle` — and on an `OPEN`, `handle` **is** the open flags.
Before this task the worst that could do was open a path read-write that the
generator had picked for reading, which nothing then wrote to. With
`KORU_O_CREAT` and `KORU_O_TRUNC` in the whitelist it could have truncated
`/etc/hostname` or the check's own pattern file, neither of which is inside the
sandbox.

The fix is `names_own_path`, and the shape of it is the point: it reads the
**slot's actual bytes** and the SQE's actual `slot`, `off` and `len`, not what
the generator meant. A mutated `off` or a shortened `len` names a prefix of
something else, which is exactly how T26's bug worked. Outside the sandbox only
a read-only open survives the mutation; access mode 3 is exempt, because it
never reaches `filp_open` at all.

### What was shown to fail

Six perturbations, each applied and reverted, each failing exactly the tests it
should and no others.

- **The `O_EXCL`-without-`O_CREAT` check deleted.** One test.
- **The mode mask never firing.** The setuid case and the above-the-mask case,
  and nothing else.
- **The mode read but not passed to `filp_open`.** The two mode assertions;
  every creation still works, which is what makes the mode worth asserting.
- **`O_CREAT` not translated.** Eleven tests, the whole section.
- **`O_TRUNC` not translated.** Two.
- **`O_APPEND` not translated.** One.

A seventh perturbation was accidental and worth recording: `mv` restoring a
perturbed file preserves its mtime, so `make` skipped the rebuild and the guest
ran the *previous* perturbation's module. It presented as a test that passed
alone and failed in the full run. **After restoring a source file by hand,
`touch` it.**

## A slot race that was passing on luck

Five tests in the two suites had the same shape: submit a deferred op and an op
behind it on the same slot, and assert the second gets `-EBUSY`. They had always
passed. They were passing because a whole-slot `CHECKSUM` in a kworker usually
takes longer than the submit loop takes to reach the next SQE — usually, not
always. Adding a test ahead of them was enough to shift the timing: the Rust
suite then failed about one run in ten, in a different test each time.

Nothing was wrong with the kernel. The assertion was simply stronger than the
ABI promises: if the first op retires before the second is validated, the second
gets the slot fairly, and that is a legal outcome. Both suites now round the
pair — 64 times — and assert that the window is *reached at least once*,
plus the invariant that holds every round: a result is either the refusal or
a real success, never a wrong answer. `Mapped::slot_race` in the Rust suite and
`race_round` in `test/koru_test.c` are the shared shapes; the same treatment
covers the two-`READDIR`-on-one-handle case, where the contended resource is the
handle rather than a slot, and the duplicate-slot `CHECKSUM` batches, where the
refused count was asserted exactly.

Deleting `OPEN`'s slot claim still fails the rounded test, so the teeth are
intact. Thirty consecutive runs of the Rust suite and five of `koru_check` pass
after the change; before it, one in ten failed.

The general lesson is the one the project keeps relearning from the other
direction: **a test that depends on losing a race has to loop until it wins.**
The cancel-during-execution window has always been written that way — Notes
records it as one to three hits per thousand rounds — and these five were the
same kind of window written as a single try.

## Braam's operation layer in Rust

T30 is `rust/runtime/src/ops.rs`: the twenty-five calls of Braam's
`src/proc/io.h` that this substrate can carry, with their signatures unchanged.
Each is a slot acquisition, a submit, an await and a copy out.

### Four things koru does differently, and why each is honest

**`seek_fd` is an assignment.** `READ` and `WRITE` carry an explicit file offset
and never touch `f_pos`, so there is nothing in the kernel to move: the position
is a number this process owns, in the ambient ring's handle record. This is the
one place koru's design makes a POSIX concept unnecessary rather than merely
different, and the binding's own documentation says so.

**`dup_fd` is a reference count.** koru has no `DUP` opcode and needs none.
Braam's contract for the operation is "one handle behind both, so a file's
offset is shared and closing one shuts nothing" — which is precisely what
returning the same handle with a `refs` bump gives, with `close_fd` issuing the
`CLOSE` only when the last name goes. The semantics are not approximated here;
they are the definition.

**`truncate_fd` goes by path.** koru's `TRUNCATE` names a path and there is no
handle form, so the operation uses the path `open_at` recorded for that handle.
Two consequences, both documented at the call site: a handle nothing opened by
name — an adopted descriptor, a standard stream — is `Err(Unsupported)`,
which is what Braam's `seek_fd` says about the same set; and a file renamed
between the open and the truncate would be truncated at its *new* occupant of
the old name. That is a genuine difference from `ftruncate(2)` and the only
gap in the layer. Closing it means a handle form of `TRUNCATE`, which is kernel
work nothing yet needs. The access-mode check is not the VFS's here — it is
ours, from the recorded open flags, because a path-named truncate would
otherwise succeed on a read-only handle whose path is writable.

**`stat_of(path, false)` is an `lstat` composed out of `READLINK`.** `STATX_AT`
always follows a final symlink and koru has no `lstat`. But `READLINK` does not
follow, so a successful `READLINK` *is* "this is a symlink", and the length it
returns is what `lstat(2)` reports as the size. `EINVAL` from it means "not a
link", and then following changes nothing, so the `STATX_AT` is exact. The only
thing lost is a symlink's own mtime, which reads as 0 — and Braam's `DirEntry`
for a link says nothing better. No kernel change was needed.

### Where Rust is stricter than Braam, deliberately

Braam's `String` is *declared* UTF-8 and not checked; Rust's is checked. So
`read_chunk`, `read_some`, `read_file`, `read_link` and `cwd_get` answer
`Err(Invalid)` on bytes that are not UTF-8, where Braam's would hand back the
bytes. That is the right trade for a text-oriented surface, and it costs nothing
where it would matter: `copy_file` and `copy_tree` never build a `String` at
all. They move bytes slot to slot through the arena, which is both binary-safe
and one copy fewer.

`errln` uses `Kind::name()` — Braam's `error_name`, word for word — and not
`Display`, which falls back to the OS message. The OS message says more. It is
the wrong choice anyway, because T49 compares the two bindings' output byte for
byte and only the name is shared.

### `open_at` speaks Braam's flags, not koru's

Braam's `SYS_O_READ`/`SYS_O_WRITE` are two independent bits; koru's access mode
is a two-bit field. The surface exposes `O_READ`, `O_WRITE`, `O_CREATE`,
`O_TRUNC`, `O_APPEND` and `O_EXCL` with Braam's own values and translates, so a
Braam call site compiles unchanged. A bit outside `O_ALL` is `Err(Invalid)`.
Braam's filesystem has no modes and so its `open_at` has no argument for one;
created files get `CREATE_MODE`, which is `creat(2)`'s `0o666` before the umask.

`open_at` also spends one `STAT` learning whether the thing it opened is
seekable, because only the creator of a handle knows that, and `write_all`'s
offset bookkeeping and `seek_fd` both depend on it. One extra op per open is the
price; nothing else in the layer pays it.

### `POLL_ADD` retires the `EAGAIN` backoff

T21's `write_all` slept a millisecond and retried when a non-blocking write said
`EAGAIN`, with a comment saying `POLL_ADD` would replace it at T22. T22 landed
and the comment outlived it. The read and write paths now arm a one-shot
`POLL_ADD` and await it. A regular file is on no waitqueue and completes at once
with `res` 0, which is why the caller retries the transfer rather than trusting
the returned mask.

### The time zone, which has no opcode and no std API

`clock_now`'s `tz_min` is the local offset from UTC. Braam asks the browser;
koru has no opcode for it, and Rust's standard library has no local-time API at
all. `rust/runtime/src/tz.rs` reads the host's TZif file (RFC 8536) — `$TZ`
where it names a zone, else `/etc/localtime` — finds the last transition at or
before now, and reports that type's offset. Anything unreadable or unparseable
is 0, which is what UTC looks like anyway.

The oracle is `date +%z` under `TZ`, run as a child process: a reader that
agrees with itself proves nothing. Six zones, including a half-hour offset
(Kolkata), a quarter-hour one (Chatham) and two whose summer time moves. Made to
fail by taking type 0 instead of the transition's, which reports Los Angeles's
1883 local mean time, −472 minutes.

### The done test, in two halves

**Signatures.** `braam_prototypes` in `rust/runtime/tests/ops.rs` is an `async
fn` that is never called: it names every operation with typed arguments and an
explicitly typed awaited result, with the C++ prototype in a comment above each
line. A drift in a name, an argument order or a type is a compile error rather
than a surprise at a call site. This is the whole of what the plan asked for and
it costs one function.

**Behaviour.** Nineteen tests against libc on a fixture tree, each owning its
own corner of `/tmp/koru-ops` so libtest's ordering does not matter. `stat_of`
against `std::fs::metadata` and `symlink_metadata`, `list_dir` against
`read_dir` field for field, `copy_file` against a file of every byte value two
slots long, `cwd_get` against `current_dir`, `seek_fd` against `lseek`'s own
three forms including the negative and past-the-end cases.

Five perturbations, each failing exactly the right tests: `close_fd` ignoring
the reference count; `SEEK_END` using 0 rather than the size; `stat_of` ignoring
`follow`; a listing keeping `.` and `..`; `truncate_fd` not checking the access
mode. The listing one fails four tests rather than one, because a `.` entry
breaks the recursive removal and the tree copy as well — which is a better
result than one, since it says those two are reading the listing for real.

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
  T41's done test is exactly this.
- **`await_suspend` must not touch `this` after publishing the handle.** Once
  the handle is visible to the reactor, the coroutine may already have been
  resumed and its frame destroyed. Harmless under the single-threaded executor,
  fatal the moment a waiter thread appears (T51) — so write the rule down now,
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
- **A blocking `OPEN` can still stall the whole ring, if the caller lets it.**
  It runs inline, so it holds `submit_lock` for the rest of its batch and every
  other submitter waits behind a slow path lookup. That is the direct price of
  resolving in the submitting task's context. T18 made it avoidable rather than
  unavoidable: `KORU_O_NONBLOCK` returns at once from a peerless FIFO, where no
  file-type check could have helped, since `filp_open` blocks before we ever see
  the file. What is left is self-inflicted by a caller who does not pass the
  flag, and a slow path lookup on a regular file, which nothing can help.
- **The arena is per fd and unaccounted**, so N open fds is N × the arena cap of
  unreclaimable memory. The device node is `0600 root:root`, which is the only
  thing bounding it.
- **Handles are per-ring**, so two rings in one process cannot share an open
  file. Nothing needs it yet.
- **`truncate_fd` truncates a path, not a handle.** `TRUNCATE` has no handle
  form, so the surface uses the path `open_at` recorded. A rename between the
  open and the truncate retargets it, and a handle nothing opened by name
  answers `Err(Unsupported)`. Closing this means a kernel change.
- **A symlink's own mtime is unreachable.** `stat_of(path, false)` composes an
  `lstat` out of `READLINK`, which gives the kind and the size exactly and no
  times at all, so a link's `mtime` reads as 0. `STATX_AT` always follows.

## Files

In dependency order. The kernel files marked *exists* are written; the rest
arrive with their tasks.

- `kernel/koru_abi.rs` — `#[repr(C)]` SQE/CQE/params/ioctl definitions.
  Mirrored byte-for-byte by `rust/sys/src/abi.rs`. The most consequential
  file in the project. *exists*
- `kernel/koru.rs` — `MiscDevice` impl, the `Arc<RingCtx>` graph, admission
  control. *exists*
- `kernel/koru_ops.rs` — opcode dispatch, SQE validation, `OpWork`, and the raw
  `bindings::` calls for `filp_open`/`kernel_read`. *exists*
- `kernel/koru_arena.rs` — `KVec<Page>`, the `mmap` validation matrix, the
  `vm_insert_page` loop, slot busy tracking. The arena code is still in
  `koru.rs`; split it out when it grows enough to be worth the churn.
- `rust/sys/src/abi.rs` — the userspace mirror of `koru_abi.rs`, with the
  same assertions as compile-time `const` checks. *exists*
- `rust/sys/src/sys.rs` — the fourteen hand-declared libc prototypes, the
  `_IOC` encoding and the typed ioctl wrappers. *exists*
- `rust/sys/src/error.rs` — `Errno`, the fifteen-name `Kind`, `Error` and
  `KORU_ERRNOS`. *exists*
- `rust/sys/src/ring.rs` — `Ring`, `Arena`, the `ENTER` outcome types and
  the `Sqe` constructors. *exists*
- `rust/sys/src/pool.rs` — `BufPool` and move-only `BufSlot`. *exists*
- `rust/sys/tests/kernel.rs` — the device suite, T4-T11 plus the T3
  matrices. *exists*
- `rust/runtime/src/slab.rs` — the generational `Cookie` and the payload-generic
  slab. Device-free, so its mechanics are host tests. *exists*
- `rust/runtime/src/op.rs` — the op states and their transitions, plus the stall
  predicate. Also device-free. *exists*
- `rust/runtime/src/reactor.rs` — the pending batch, `ENTER`, dispatch, and the
  lock order everything else obeys. *exists*
- `rust/runtime/src/exec.rs` — `Runtime`, the task slab, the waker, `spawn`,
  `block_on`. *exists*
- `rust/runtime/src/future.rs` — one future per opcode and their shared drop.
  *exists*
- `rust/runtime/src/combinator.rs` — `race` and `Either`, and the only place a
  koru future is dropped for you. *exists*
- `rust/runtime/tests/runtime.rs` — the device suite for all of it. *exists*
- `rust/runtime/src/vocab.rs` — Braam's fifteen names and three aliases over
  koru-sys's own `Error`, never a second type. *exists*
- `rust/runtime/src/rt.rs` — the ambient ring, the handle records that carry a
  position, a reference count and a path, and the runtime entry. *exists*
- `rust/runtime/src/args.rs` — Braam's `Args`. *exists*
- `rust/runtime/src/ops.rs` — Braam's operation layer, T30. *exists*
- `rust/runtime/src/tz.rs` — the TZif reader `clock_now` needs, because there
  is no opcode and no std API for a local time offset. *exists*
- `rust/runtime/tests/ops.rs` — Braam's prototypes, and the operation layer
  against libc on a fixture tree. *exists*
- `cpp/include/koru_abi.h` — the C mirror of `koru_abi.rs`, kept in step by
  the T14 conformance diff. *exists*
- `cpp/include/koru_errno.h` — the C mirror of `error.rs`'s vocabulary and
  errno table, as X-macros. *exists*
- `cpp/tools/abi_dump.cpp` and `rust/sys/src/bin/abi_dump.rs` — the two
  emitters `scripts/abi.sh` diffs. *exists*
- `cpp/include/koru.hpp` — `Ring`, `BufPool`, move-only `BufSlot`,
  `result<T>`.
- `cpp/include/koru/task.hpp` — `task<T>` promise type, symmetric transfer,
  `sync_wait`.
- `cpp/include/koru/awaiter.hpp` — op slab, `op_awaiter`, the abandonment
  path.
- `cpp/examples/read_file.cpp` — the C++20 demo.

`test/koru_check` stays the kernel's own check and is not superseded by the
Rust suite: it owns the fuzz, the two `rmmod` races and the heavy-phase leak
window. A wire-format change is still three files — `kernel/koru_abi.rs` and
the two userspace mirrors, `rust/sys/src/abi.rs` and
`cpp/include/koru_abi.h` — but only the first of those three is now unchecked.
`test/` includes the C mirror rather than keeping a fourth copy, and
`scripts/abi.sh` diffs the two mirrors against each other.

## Why the tasks are ordered this way

**Control plane before memory plane.** The natural instinct is to mmap first
because that is the boundary, but that front-loads all the exploratory MM risk
and leaves nothing demoable if `vm_insert_page` turns into a swamp. Inverted, we
had a fully working async protocol at T6 — and the ABI, the part we would most
regret getting wrong, was settled while it was still cheap to change.

T6 was the first demoable milestone: a working async syscall interface.
Everything after it is realism or optimization.
