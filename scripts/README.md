# scripts — running the check

`test/koru_check` is the whole test suite for the kernel module: one binary, one
shared ring, one process. `scripts/check.sh` runs it inside a virtme-ng guest
along with the handful of checks that need a second process, and prints one
verdict. `scripts/run.sh` boots that guest from the host.

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

## Adding a check

Put it in the section it belongs to, or add a section to the table in
`test/koru_check.c`, marking it heavy if it allocates in bulk. Assert the exact
errno, never just that a call failed. Then break the thing it guards and watch
that check, and only that check, fail. A check that has not been shown to fail
proves nothing; `doc/Notes.md` records which ones have earned that and which
have not.
