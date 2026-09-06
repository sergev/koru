# xring

An experimental Linux kernel API built for coroutines instead of POSIX.

## The problem

A POSIX syscall is a rendezvous. You execute a trap instruction, control leaves your
program, and it comes back at the same place. That is exactly wrong for a coroutine, which
must yield to its executor *between* the call and the answer.

So a coroutine-friendly API cannot be a trap. Submission and completion have to be
separate events. That means a queue, not a call.

## The idea

Open `/dev/xring`. You get a ring: push operations in, pull completions out, and resume
whichever coroutine each completion belongs to.

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

No `errno`, no file descriptors, no POSIX compatibility. Operations carry a `user_data`
cookie; completions hand it back so you know which coroutine to wake.

## The one rule that shapes everything

> **A coroutine can never own kernel-visible memory.**

Here is why that matters. A coroutine can be destroyed at any suspension point — a
timeout fires, a `select!` picks the other branch, a task is cancelled. If an in-flight
operation is holding a pointer into that coroutine's memory, the kernel is now writing
into freed memory. There is no way to block in a destructor to wait for the kernel, in
Rust or in C++.

So we remove the possibility. All I/O memory lives in a **buffer arena** that the *kernel*
allocates and owns, mapped into your process. An operation names a buffer by **slot
index**, never by pointer:

```rust
read(handle, slot: 3, len: 4096)   // not: read(handle, ptr, len)
```

The kernel bounds-checks the index and touches only pages it allocated itself. It never
dereferences a userspace address. Nothing your program does — drop, `free`, `munmap`,
`close`, `kill -9` — can invalidate the kernel's target.

Use-after-free isn't prevented here. It's unrepresentable.

## What it looks like

Rust:

```rust
let h = ring.open("/etc/hostname").await?;
let (n, buf) = ring.read(h, buf).await?;   // buf moves in, comes back out
println!("{}", str::from_utf8(&buf[..n])?);
ring.close(h).await?;
```

C++20:

```cpp
auto h = co_await ring.open("/etc/hostname");
auto [n, buf] = co_await ring.read(h, std::move(buf));
std::cout << std::string_view{buf.data(), n};
co_await ring.close(h);
```

Same ABI underneath, no kernel code in common with either binding. C++20 coroutines
actually fit it slightly better than Rust futures: they're continuation-based, so a
completion resumes a handle directly with no polling and no `Waker`.

## How it's put together

| Piece | What it does |
|---|---|
| Kernel module (Rust) | misc device, opcode dispatch, buffer arena, worker offload |
| `ENTER` ioctl | submits operations and blocks for completions — the only entry point |
| `mmap` arena | fixed-size slots in kernel-owned pages; the data plane |
| `user/xring` | Rust binding: `Future` impls + executor |
| `user/cpp` | C++20 binding: awaiters, `task<T>`, executor |

The coroutines are entirely in userspace. Kernel Rust has no async runtime, so the kernel
side is a dispatch table plus a worker pool — the same split io_uring makes with `io-wq`.

## Status

**Design only. No code yet.** [Plan.md](Plan.md) has the full design and a task list
(T0–T23) with a test for each step.

Requires a Linux box running a kernel ≥6.16 built with `CONFIG_RUST=y` — and the full
build tree, since distro `linux-headers` packages omit the Rust artifacts. Use a VM;
early versions will panic it.

## Prior art

[io_uring](https://kernel.dk/io_uring.pdf) is the working proof that Linux's async
interface is a ring rather than a trap; ublk and NVMe passthrough show how to drive
arbitrary operations through one. [FlexSC](https://www.usenix.org/legacy/event/osdi10/tech/full_papers/Soares.pdf)
(OSDI 2010) is the academic ancestor, and found what you'd expect: the win comes from
batching and cache locality, not from dodging the trap instruction.
