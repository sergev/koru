# donetest — the VM-side test harness

Every task in [doc/Plan.md](../doc/Plan.md) carries a *done test*: a script that
decides whether that task is actually finished. A task is done when its done
test passes **and** when the test has been shown to fail if the thing it checks
is broken. These are those scripts.

They run inside a virtme-ng guest, never on the host. A module bug panics the
machine, and the host is somebody's desktop.

## Running them

```sh
donetest/run.sh                    # every test, in order
donetest/run.sh 9 10 11            # just those
DT_ARGS="30 999" donetest/run.sh 12   # arguments for a single test
KDIR=/path/to/tree donetest/run.sh    # a kernel tree somewhere else
```

`run.sh` is the only part that runs on the host. It boots the kernel in a VM,
runs the guest script inside it, and prints one verdict line per test. It exits
non-zero if any test did not pass, so it works as a gate.

`KDIR` defaults to `../kernel-dev/linux-source-7.1`. `MEMORY` and `CPUS`
override the VM's size.

A single test can also be run by hand, which is what you want when you need the
full output rather than a verdict:

```sh
vng --run $KDIR --user root --memory 4G --cpus 4 --exec "sh donetest/t9.sh"
```

Build first. `run.sh` does not build anything, on purpose: it should test what
is on disk, not silently rebuild it.

```sh
make -C $KDIR M=$PWD/kernel LLVM=1
make -C test
```

## Reading the result

Each script prints exactly one marker as its last line, and that marker is what
`run.sh` greps for.

- `TN-DONE-TEST-PASS` — the task's done test passed.
- `TN-DONE-TEST-COMPLETE` — same thing, for the three early tests that predate
  the pass/fail convention and report rather than assert.
- `TN-DONE-TEST-FAIL` — something failed. The script prints what.
- No marker at all means the guest died or the test hung. `run.sh` says so and
  dumps the tail.

## What each script covers

**t0.sh** — the dev kernel itself, not koru. Loads and unloads
`rust_minimal`, and confirms KASAN, lockdep and kmemleak are genuinely live
rather than merely configured. Run it after any kernel config change;
`merge_config.sh` drops unmet options silently, so "I set it in the fragment" is
not evidence.

**t1.sh** — insmod and rmmod the module, check the init and exit prints, and
check the taint word. Expected taint is exactly 4096, `TAINT_OOT_MODULE`.
Module signing is off in this kernel, so bit 13 must never appear.

**t2.sh** — 10,000 open/close iterations, and the device node's permissions.
Scans kmemleak twice, once with the module loaded and once after unload. The
post-unload scan is the authoritative one: anything still unreferenced when the
module is gone has nobody left to free it.

**t3.sh** — `SETUP` and `GET_PARAMS`. The parameter round-trip, rejection of a
version mismatch, and a second `SETUP` returning `EBUSY`.

**t4.sh** — `ENTER` and `NOP`. Eight submissions in, eight completions out, and
a garbage opcode producing a completion rather than failing the ioctl.

**t5.sh** — `DELAY_NS`: concurrency, the timeout, and the interruptible wait.

**t6.sh** — teardown and module pinning. Closing the fd with work in flight,
killing a blocked task, `rmmod` refused while an fd is still open, and `rmmod`
succeeding *promptly* once the fd closes. The timing matters as much as the
outcome there: waiting the queued delays out would also end with `rmmod`
succeeding, so the script fails if it took more than a couple of seconds.

**t7.sh** — the mmap'd arena and `CHECKSUM`, plus the mapping matrix:
`MAP_SHARED` required, exact length, zero offset, one-shot, and no inheritance
across `fork`.

**t8.sh** — slot exclusivity. Two concurrent operations naming one slot, and the
slot becoming free exactly when the completion posts.

**t9.sh** — `OPEN` and `CLOSE`, the generational handle table, and the absence
of an fd leak. Also the creds regression: an unprivileged submitter must get
`EACCES` on `/etc/shadow`, which is what proves the open resolves in the
submitting task's context rather than the kernel's.

**t10.sh** — `READ` into a slot. The bytes must match `read(2)`, a read past EOF
must be short rather than an error, and a `CLOSE` racing an in-flight `READ`
must be clean under KASAN.

**t11.sh** — `CANCEL`. A cancelled `DELAY_NS` returns at once with the target
completing `ECANCELED`, and the elapsed time is asserted: without a real dequeue
the test would simply wait the delay out and still see the right result.

**t12.sh** — the hostile-userspace fuzz, and the longest by far. Ten minutes by
default; pass a shorter duration while iterating. Random and adversarial
submissions from eight threads on one ring, faulting buffers, malformed ioctl
numbers, `munmap` under in-flight work and children taking `SIGKILL`. The
fuzzer prints its seed first, and passing that seed back replays the run.

## common.sh

Sourced by every script; never run directly. It holds the parts that are
identical everywhere: insmod, running a test binary, the kmemleak scan, rmmod,
the taint check, the splat grep, and the verdict.

Keep it small and obvious. Every done test's pass condition runs through it, so
a bug here weakens all of them at once. That is the cost of not having thirteen
copies, and the copies had already caused one silent breakage and three
fix-everywhere edits.

Two rules it encodes are worth knowing, because both were learned the hard way.

**kmemleak reports nothing about an object younger than five seconds**
(`MSECS_MIN_AGE` in `mm/kmemleak.c`). Scanning right after a test loop therefore
reports a clean result no matter how badly the code leaks. `dt_kmemleak` sleeps
past that age and then scans twice, because the first pass after heavy
allocation is not settled.

**A kernel splat must fail the run, not merely print.** T10 shipped a real
lockdep deadlock behind a `PASS`, with the cycle printed directly above the pass
line, because the dmesg check was advisory. `dt_splat` captures it and the
verdict gates on it being empty.

The repo root comes from the calling script's own path. Do not hardcode an
absolute path: that is what silently broke all thirteen scripts when the project
was renamed, and it went unnoticed for four commits.

## koru-debug.config

The kernel config fragment for the dev kernel: KASAN generic, inline and
vmalloc, `PROVE_LOCKING`, `DEBUG_KMEMLEAK`, `DEBUG_OBJECTS`, DWARF5 and
`SAMPLE_RUST_MINIMAL=m`. `MODVERSIONS`, `MODULE_SIG`, `DEBUG_INFO_BTF` and
`RANDSTRUCT` are off on purpose.

It lives here rather than next to the kernel tree because it is a project
decision, not a build artifact. The tree it configures stays outside the repo:
it is about 5.4 GB and takes nine minutes to build.

After changing it, re-run `t0.sh`. Options that fail to apply are dropped
without a word.

## Adding a done test

Most tasks need only the thin form:

```sh
#!/bin/sh
# TN done test, run inside the virtme-ng guest.
# Done test: one line on what finishing this task means.

. "$(dirname "$0")/common.sh"

dt_begin "TN" "short label"
dt_insmod
dt_run "test/tN_thing"
dt_finish
```

A test with its own shape calls the pieces directly, as `t2.sh` and `t6.sh` do,
setting `dt_fail=1` on its own failures and ending with `dt_verdict`. Add the
number to the list in `run.sh`.
