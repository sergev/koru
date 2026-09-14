# scripts — running the check

`test/koru_check` is the whole test suite for the kernel module: one binary, one
shared ring, one process. `scripts/check.sh` runs it inside a virtme-ng guest
along with the handful of checks that need a second process, and prints one
verdict. `scripts/run.sh` boots that guest from the host. Two more gates sit
beside it: the Rust suites in their own guest, and the ABI diff, which needs no
guest at all.

Never load the module on the host. A module bug panics the machine, and this is
somebody's desktop.

## Running it

Build first. The runner deliberately does not build, so that a stale binary is
an error rather than a silent rebuild in the middle of a debugging session.

```sh
make -C ../kernel-dev/linux-source-7.1 M=$PWD/kernel LLVM=1
make -C test
```

Then:

```sh
scripts/run.sh                      # the whole check, about 27 seconds
scripts/run.sh open read cancel     # just those sections
KORU_SEED=12345 scripts/run.sh      # replay a fuzz failure
KDIR=/path/to/tree scripts/run.sh   # a kernel tree somewhere else
```

`MEMORY`, `CPUS` and `TIMEOUT` are also overridable. The default memory is 2 GB,
which halves the kmemleak scan relative to 4 GB and changes nothing else.

Inside a guest you already have, or for a shell to poke at:

```sh
vng --run $KDIR --user root --memory 2G --cpus 4 --exec "sh scripts/check.sh"
vng --run $KDIR --user root --memory 2G           # interactive
```

`test/koru_check --list` prints the section names. Running a subset skips the
leak window arithmetic, so use the full run before believing a clean result.

## Reading the result

`run.sh` exits zero only on `KORU-CHECK-PASS`, so it works as a gate. On
anything else it prints the last section banner reached, the failing check
lines, and the tail. No verdict at all means the guest died or hung.

Every line the binary prints is `<description> PASS` or `<description> FAIL`,
with indented `    ` lines for context that is reported rather than asserted:
race-arm distributions, fuzz counters, file descriptor numbers.

## What the check covers

Sections run in a fixed order, printed as `== SECTION <name> ==`.

`smoke` first: a NOP, the arena reading back zeroed, and one CHECKSUM. A module
that is fundamentally broken fails here in a tenth of a second rather than
somewhere inside the fuzz.

Then the **heavy phase**, which is everything that allocates in bulk:
`devchurn` opens and closes the device 2000 times; `ringchurn` builds and tears
down 300 rings with rotating geometry, two thirds of them abandoned with work
still queued; `handles` runs 500 open/close pairs and the release drain, with
descriptor accounting; `fuzz` is three seconds of hostile userspace in a forked
child; `races` is the four race loops.

Then the **tail**: `setup`, `ioctl`, `mmap`, `enter`, `slots`, `checksum`,
`open`, `read`, `delay`, `cancel`, `signals`, `creds`. These are deterministic
matrices plus the four timing assertions, and they allocate almost nothing.

**The ordering is load-bearing, and anything added has to respect it.** kmemleak
reports nothing about an object younger than five seconds, so the heavy phase
runs first and the binary stamps `KORU-HEAVY-END-MS` as its last line;
`check.sh` measures the age of that stamp and tops the window up only by the
shortfall. A new loop that allocates in bulk belongs in the heavy phase. Put one
in the tail and the single scan cannot see what it leaks.

After the binary, `check.sh` runs the two properties that need a second process:
`rmmod` must be refused while a descriptor is open, and after a process exits
with four five-second delays still queued, `rmmod` must succeed within two
seconds. The timing there is the assertion, not the outcome: waiting the delays
out would also end with `rmmod` succeeding.

Then the leak scan, the taint check, and the dmesg scan.

## Three rules the verdict depends on

**The dmesg scan gates, it does not merely print.** T10 once shipped a real
lockdep deadlock behind a PASS because that check was advisory. It also matches
`not supported for file`, which is a `pr_warn_ratelimited` rather than a WARN
and so matched nothing until it was named. It filters out this script's own
`/dev/kmsg` fences, which would otherwise match their own pattern.

**Taint must be exactly 4096**, `TAINT_OOT_MODULE`. Module signing is off in
this kernel, so bit 13 must never appear; anything else is a finding.

**A missing debug facility fails the run.** Without that, a kernel built without
KASAN, lockdep or kmemleak would quietly pass everything.

## koru-debug.config

The fragment that built the dev kernel: KASAN, lockdep, `DEBUG_ATOMIC_SLEEP`,
kmemleak, the debug-objects family. Deliberately off: `MODVERSIONS`,
`MODULE_SIG`, `DEBUG_INFO_BTF`, `RANDSTRUCT`, `SHADOW_CALL_STACK`. After any
change, re-check that the options survived `olddefconfig` — `merge_config.sh`
drops unmet ones silently.

## The Rust suites

`scripts/rust.sh` and `scripts/run-rust.sh` are the same shape for the Rust
integration tests: one VM boot, one verdict line `KORU-RUST-PASS`, and the same
insmod, kmemleak, taint and dmesg gates. `check.sh` is untouched, because it
carries every pass condition for the kernel and is meant to stay small.

Five suites run in that one boot, listed in `SUITES` at the top of
`run-rust.sh` as `package:target:floor`: `koru-sys`'s `kernel`, the device
matrix, and `koru`'s `runtime`, `ops`, `file` and `screen` — the futures and
the executor, the operation layer against libc, the buffered `File`, and the
screen client against a fake daemon over a socketpair.

```sh
(cd ../rust && cargo test --workspace --no-run)   # build first, as above
(cd ../rust && cargo build --examples)            # and the example programs
scripts/run-rust.sh
scripts/run-rust.sh cancel read                   # only matching test names
KORU_SEED=12345 scripts/run-rust.sh               # replay a race loop
KORU_ITERS=100000 TIMEOUT=2400 scripts/run-rust.sh drop_safety
```

`KORU_ITERS` and `KORU_SEED` are forwarded into the guest, and every test that
uses them prints the pair it ran with. A race loop's default count is low so
the everyday gate stays fast; the last line above is T16's full hundred
thousand rounds, which take about fourteen seconds of guest time and need a
`TIMEOUT` past the 600-second default. Note that any name filter trips the
per-suite floor below, so a filtered run always reports `TOO FEW TESTS RAN`.

Two gates exist that `check.sh` does not need, because `cargo test` can succeed
without running anything. A filter matching nothing exits 0, so each suite
carries a minimum passed count, raised when a test is added. The floor is per
suite on purpose: one total would let one crate's growth mask a filter typo
that ran none of another's. And a skipped precondition prints `KORU-RS-SKIP`,
which fails the run rather than passing quietly.

It pays a flat six-second kmemleak window instead of the stamp arithmetic
above: libtest orders tests by name, so neither suite can promise the
heavy-first ordering that budget depends on. The kernel is unchanged, so leak
coverage is still `koru_check`'s job.

It picks each test binary out of `cargo`'s JSON by target **kind**. Both
packages carry a dump bin the same command emits — `koru-sys`'s `abi_dump`
since T14, `koru`'s `ks_dump` since T33 — in an order that varies; taking the
wrong one runs no tests and exits 0.

After the suites it runs the **examples**, listed in `EXAMPLES` and located the
same way. They are the runtime entry's only test, because `#[koru::main]`
replaces `main` and libtest cannot call one. `hello` runs three ways, and each
way asserts something different: through a pipe, which koru can only write to
after re-opening it; redirected to a regular file, which is three writes at
three offsets; and with an argument, which is `Args`. `date` is T32's usage
helpers, which no library test can see: a block on stdout and status 0, the
same block on stderr and status 2, and the calendar against the host's own
`date(1)` read either side of ours. A filtered run skips the examples, since a
filter is a test-name filter, and an unfiltered run with none fails.

**A koru program run straight onto the VM console prints nothing.** Its stdout
is a virtio-serial port, which permits one open, so the runtime cannot re-open
it non-blocking and the kernel refuses the write. Redirect or pipe it — which
is what the gate does, and what doc/Notes.md explains.

## The screen gate

`scripts/screen.sh` builds `build-asan/` with ASan and UBSan on and runs the
terminal model's suite — Braam's own cell-exact assertions, ported — the five
pixel oracles, the protocol server's rejection matrix, and two fuzz oracles,
one over the ANSI parser and one over the protocol's `feed()`. None of it needs
a VM, a device or a module; the pixel oracles need SDL3 and run under its
offscreen video driver, which the test sets for itself, so no display is needed
either.
About a second.

```sh
scripts/screen.sh
KS_SEED=12345 scripts/screen.sh     # replay a fuzz failure
KS_ITERS=500000 scripts/screen.sh   # a longer fuzz run
CXX=clang++ scripts/screen.sh       # once libclang-rt-*-dev is installed
```

Where SDL3 is installed the pixel oracles are not optional: the script asks
`pkg-config` and fails if they did not run, because a build that quietly
dropped them would pass everything else.

It defaults to GCC because clang's sanitizer runtimes are a separate Debian
package. The coverage-guided libFuzzer target is opt-in for the same reason:
`cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DKORU_FUZZ=ON`, then
`./build-fuzz/ks_ansi_fuzz -max_total_time=30`. Both drivers call the same
`LLVMFuzzerTestOneInput`, so they share one oracle and differ only in how they
pick inputs.

The verdict is `KORU-SCREEN-PASS`, and it gates on more than the exit status:
the suite's own "OK: 0 failure(s)" line and its section banners must appear, or
a binary that ran nothing would pass.

## The screen end to end

`scripts/run-e2e.sh` boots the VM and runs `scripts/e2e.sh` in it: the spawn
race, the lifecycle at both ends, the byte channel, and Braam's `less` painting
a real window under SDL's offscreen driver. It is the only gate that needs all
three of the module, SDL and a runtime directory, which is why it is its own.

```sh
cmake -B build && cmake --build build      # the daemon and ks_pixel
(cd rust && cargo build --examples)        # less, hello, date and screen_probe
scripts/run-e2e.sh
```

The verdict is `KORU-E2E-PASS`. What it asserts, in order: twenty clients
racing from nothing produce exactly one daemon and all twenty connect; twenty
more against a live daemon leave its pid alone; a socket whose daemon was
killed is replaced; a client whose daemon is killed exits non-zero saying the
connection is closed; a client killed mid-blit gives the screen back and the
next one is served; `hello` and `date` print on the window rather than on the
terminal they were started from, while a redirected stdout is untouched; and
`less` paints a status line whose modal colour is the palette's cyan, which is
a pixel assertion rather than "it ran".

The byte channel's cases need a terminal to be started from, which `script`
supplies: the child's stdout is a pty, so what the pty saw is exactly what
would have been printed where koru was started, and an empty capture is the
assertion. Every row is measured from its **second** cell, because the first is
where the block cursor sits and 320 pixels of cursor would make a blank row
look written on.

Two rules the counting depends on, both learned the hard way. The daemon is
counted by an **exact** command line, because this script's own environment
carries the daemon's path and `pgrep -f` would match the shell. And each client
writes to a **file of its own**, because twenty processes appending to one file
braid their lines.

## The C++ binding's device suite

`scripts/run-cpp.sh` boots the VM and runs `scripts/cpp.sh` in it, which drives
`build/koru_cpp_check` — the T4-T11 matrix and the T3 matrices in C++, case for
case with `rust/sys/tests/kernel.rs`.

```sh
cmake -B build && cmake --build build
scripts/run-cpp.sh
scripts/run-cpp.sh cancel read   # only cases whose name matches
```

It also runs T15's demo in both languages and compares the bytes on both
streams, which is why it needs `cargo build --examples` as well as the module.

The verdict is `KORU-CPP-PASS`, and it gates on the same things the Rust runner
does: the suite's own "OK: 0 failure(s)" line, a floor on the number of cases
an unfiltered run reached (a filter matching nothing runs none and exits 0), a
`KORU-CPP-SKIP` marker, and then rmmod, kmemleak, taint and the dmesg scan.

## The ABI conformance diff

`scripts/abi.sh` compares two pairs of mirrors, C and Rust, by diffing a
canonical record dump emitted from each: the koru ABI (`cpp/include/koru_abi.h`
against `rust/sys/src/abi.rs`) and, since T33, the screen protocol
(`screen/ks_abi.h` against `rust/runtime/src/ks_abi.rs`). Each pair has its own
record floor. It needs no VM, no `/dev/koru` and no module, so it is the fastest
gate here — run it whenever a mirror changes.

```sh
cmake -B build && cmake --build build   # once, and after a mirror changes
scripts/abi.sh
scripts/abi.sh --cpp build-asan/abi_dump
ctest --test-dir build                  # the same script, as a ctest
```

Like the runners above it does not build; a missing `build/abi_dump` prints the
`cmake` line and fails. A bare `diff` of the two would pass when both sides
print nothing, so the script gates on both exit statuses, both lengths and a
`WANT_RECORDS` floor before it compares anything. Raise that floor when the
surface grows, for the reason `WANT_PASSED` exists.

The verdict line is `KORU-ABI-PASS` or `KORU-ABI-FAIL`.

## Adding a check

Put it in the section it belongs to, or add a section to the table in
`test/koru_check.c`, marking it heavy if it allocates in bulk. Assert the exact
errno, never just that a call failed. Then break the thing it guards and watch
that check, and only that check, fail. A check that has not been shown to fail
proves nothing; `doc/Notes.md` records which ones have earned that and which
have not.
