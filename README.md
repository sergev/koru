# koru

An experimental Linux kernel API built for coroutines instead of POSIX.

## The problem

A POSIX syscall is a rendezvous. You execute a trap instruction, control leaves
your program, and it comes back at the same place. That is exactly wrong for a
coroutine, which must yield to its executor *between* the call and the answer.

So a coroutine-friendly API cannot be a trap. Submission and completion have to
be separate events. That means a queue, not a call.

## The idea

Open `/dev/koru`. You get a ring: push operations in, pull completions out, and
resume whichever coroutine each completion belongs to.

```
your program            kernel
------------            ------
submit  op #7  ──────►  dispatch
submit  op #8  ──────►    (fast ops answer immediately,
co_await / .await         slow ones go to worker threads)
        ...
resume  op #8  ◄──────  complete   ← out of order, and that's the point
resume  op #7  ◄──────  complete
```

No `errno`, no file descriptors, no POSIX compatibility. Operations carry a
`user_data` cookie; completions hand it back so you know which coroutine to
wake.

## The one rule that shapes everything

> **A coroutine can never own kernel-visible memory.**

Here is why that matters. A coroutine can be destroyed at any suspension point —
a timeout fires, a `select!` picks the other branch, a task is cancelled. If an
in-flight operation is holding a pointer into that coroutine's memory, the
kernel is now writing into freed memory. There is no way to block in a
destructor to wait for the kernel, in Rust or in C++.

So we remove the possibility. All I/O memory lives in a **buffer arena** that
the *kernel* allocates and owns, mapped into your process. An operation names a
buffer by **slot index**, never by pointer:

```rust
read(handle, slot: 3, len: 4096)   // not: read(handle, ptr, len)
```

The kernel bounds-checks the index and touches only pages it allocated itself.
It never dereferences a userspace address. Nothing your program does — drop,
`free`, `munmap`, `close`, `kill -9` — can invalidate the kernel's target.

Use-after-free isn't prevented here. It's unrepresentable.

## What it looks like

The userspace API is [Braam](https://github.com/braamix/core)'s. Braam is a
browser-hosted operating system whose programs are wasm modules with no libc and
no stack switching, so its syscalls are a submit import plus a completion export
and every blocking call is a C++20 coroutine. Different substrate, the same
structural bet — and unlike a freshly invented API, that one already has more
than fifty programs written against it.

Rust:

```rust
let h = open_read("/etc/hostname").await?;
let text = read_chunk(h).await?;
write_all(STDOUT, &text).await?;
close_fd(h).await;
```

C++20:

```cpp
auto h    = CO_TRY(co_await open_read("/etc/hostname"));
auto text = CO_TRY(co_await read_chunk(h));
co_await write_all(STDOUT, text);
co_await close_fd(h);
```

Same ABI underneath, no code in common between the two bindings. The slot index
does not appear here: at this layer a read returns bytes you own, and the
binding does the copy. The point is that the *kernel* never saw a pointer, and
that stays true no matter what the surface looks like.

It also buys something back. `write_all` borrows its buffer across the
suspension, which on raw io_uring is unsound in Rust and is the whole reason
owned-buffer runtimes exist. Here the bytes are copied into a slot before the
operation is submitted, so the borrow ends before the await begins.

C++20 coroutines fit the ring slightly better than Rust futures: they're
continuation-based, so a completion resumes a handle directly with no polling
and no `Waker`.

## How it's put together

- **Kernel module, in Rust** — the misc device, opcode dispatch, the buffer
  arena and the worker offload.
- **The `ENTER` ioctl** — submits operations and blocks for completions. It is
  the only entry point.
- **The `mmap`'d arena** — fixed-size slots in kernel-owned pages. This is the
  data plane.
- **`rust/koru-sys`, `rust/koru`** — the Rust binding: ABI structs and ioctl
  wrappers, then `Future` impls, an executor and the Braam surface.
- **`cpp`** — the C++20 binding: awaiters, `task<T>` and an executor.

The coroutines are entirely in userspace. Kernel Rust has no async runtime, so
the kernel side is a dispatch table plus a worker pool — the same split io_uring
makes with `io-wq`.

## Status

**The kernel module works, and the Rust ABI layer is written.** It registers
`/dev/koru` and runs `NOP`, `DELAY_NS`, `CHECKSUM`, `OPEN`, `READ`, `CLOSE` and
`CANCEL` through `ENTER`. The arena is mapped and slot exclusivity is enforced
by the kernel, open files live in a generational handle table, a queued
operation can be genuinely dequeued, and closing the ring cancels whatever is
still queued. The whole validation surface has been fuzzed for ten minutes under
KASAN, lockdep and kmemleak with no kernel messages.

One binary, `test/koru_check`, is the entire test suite for the module.
`scripts/run.sh` boots a VM, runs it and prints one verdict line in about half a
minute.

`rust/koru-sys` is the first half of the Rust binding: the `#[repr(C)]` ABI
mirror, the ioctl wrappers, the ring and its arena, a buffer pool, and the errno
table. Its integration suite re-expresses the module's own tests against that
API, so the two independent views of the wire format have to agree.
`scripts/run-rust.sh` runs it in a VM. The futures and the executor are next.

The wire format now exists in a C mirror as well, `cpp/include/koru_abi.h`,
which the C test suite includes rather than copying. Each side prints a
canonical dump of the whole surface and `scripts/abi.sh` diffs the two, so the
agreement is checked rather than promised. It needs no VM and no device.

Next is userspace, and the Braam decision reopens the kernel for one phase:
there is no `WRITE` opcode yet, and `OPEN` refuses anything but regular files
and directories on purpose, because a blocking read in a worker thread cannot be
interrupted. So today a koru program cannot write to a terminal or a pipe at
all, which the target API rather depends on.

[doc/Notes.md](doc/Notes.md) has the design and the reasoning behind it.
[doc/Plan.md](doc/Plan.md) has the remaining tasks, each with a test.

Requires a Linux box running a kernel ≥6.16 built with `CONFIG_RUST=y` — and the
full build tree, since distro `linux-headers` packages omit the Rust artifacts.
Use a VM; early versions will panic it.

## License

MIT, except the kernel module. `kernel/` is GPL-2.0, because a Linux module
that uses GPL-only symbols has to be, and it declares `MODULE_LICENSE("GPL")`
to load at all. Everything else — the userspace bindings, the test suite and
the scripts — is MIT, so a program written against koru is not obliged to be
GPL by the binding it links.

`kernel/koru_abi.rs` is GPL-2.0 as part of the module, and its userspace
mirrors are MIT. Both are the same copyright holder's work, which is what makes
that split his to make.

## Prior art

[io_uring](https://kernel.dk/io_uring.pdf) is the working proof that Linux's
async interface is a ring rather than a trap; ublk and NVMe passthrough show how
to drive arbitrary operations through one.
[FlexSC](https://www.usenix.org/legacy/event/osdi10/tech/full_papers/Soares.pdf)
(OSDI 2010) is the academic ancestor, and found what you'd expect: the win comes
from batching and cache locality, not from dodging the trap instruction.
