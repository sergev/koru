#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Host-side runner for the Rust suites. Boots the dev kernel in a VM and runs
# scripts/rust.sh inside it. Never load the module on the host.
#
#   scripts/run-rust.sh                 every suite
#   scripts/run-rust.sh cancel read     only tests whose name matches
#   KDIR=/path/to/tree scripts/run-rust.sh
#
# It does not build. Do that first:
#   make -C ../kernel-dev/linux-source-7.1 M=$PWD/kernel LLVM=1
#   (cd rust && cargo test --workspace --no-run && cargo build --examples)
#
# Exits non-zero unless every suite passed, so it is usable as a gate.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=${KDIR:-$ROOT/../kernel-dev/linux-source-7.1}
MEMORY=${MEMORY:-2G}
CPUS=${CPUS:-4}
TIMEOUT=${TIMEOUT:-600}

# package:test-target:minimum-passed, one per suite. The floor is per suite on
# purpose: a single total would let one crate's growth mask a filter typo that
# ran none of another's. Raise a floor when a test is added.
SUITES=${SUITES:-"koru-sys:kernel:93 koru:runtime:25"}

# The example programs, run in the guest by rust.sh. They are the entry's own
# test: #[koru::main] replaces main, so libtest cannot call one.
EXAMPLES=${EXAMPLES:-"hello read_file"}

# Race-loop knobs, forwarded into the guest. KORU_ITERS raises a loop's count
# and KORU_SEED replays one; each test prints the values it used. The full
# 100k T16 run needs a TIMEOUT well past the default.
KORU_ITERS=${KORU_ITERS:-}
KORU_SEED=${KORU_SEED:-}

if [ ! -d "$KDIR" ]; then
	echo "no kernel tree at $KDIR; set KDIR" >&2
	exit 1
fi
if [ ! -f "$ROOT/kernel/koru.ko" ]; then
	echo "kernel/koru.ko is not built" >&2
	exit 1
fi

# Ask cargo where each binary is rather than globbing target/debug/deps, where
# stale hashes accumulate and a newest-match would survive a failed build.
# The abi_dump bin is emitted by this same command, in an order that varies,
# so select the test target by kind: picking the bin runs nothing and exits 0.
# Not "test":true, which a bin target also carries.
spec=""
for suite in $SUITES; do
	pkg=${suite%%:*}
	rest=${suite#*:}
	tgt=${rest%%:*}
	floor=${rest##*:}
	bin=$(cd "$ROOT/rust" && cargo test -p "$pkg" --test "$tgt" --no-run \
		--message-format=json 2>/dev/null \
		| grep -F '"kind":["test"]' \
		| tr ',' '\n' \
		| sed -n 's/.*"executable":"\([^"]*\)".*/\1/p' \
		| tail -1)
	if [ -z "$bin" ] || [ ! -x "$bin" ]; then
		echo "no test binary for $pkg/$tgt" >&2
		echo "run: (cd rust && cargo test --workspace --no-run)" >&2
		exit 1
	fi
	spec="$spec $tgt|$bin|$floor"
done

# Same question to cargo, for the examples: a stale binary under target/debug
# would outlive a failed build and pass.
exspec=""
for ex in $EXAMPLES; do
	bin=$(cd "$ROOT/rust" && cargo build -p koru --example "$ex" \
		--message-format=json 2>/dev/null \
		| grep -F '"kind":["example"]' \
		| tr ',' '\n' \
		| sed -n 's/.*"executable":"\([^"]*\)".*/\1/p' \
		| tail -1)
	if [ -z "$bin" ] || [ ! -x "$bin" ]; then
		echo "no example binary for $ex" >&2
		echo "run: (cd rust && cargo build --examples)" >&2
		exit 1
	fi
	exspec="$exspec $ex|$bin"
done

cd "$ROOT" || exit 1

out=$(timeout "$TIMEOUT" vng --run "$KDIR" --user root --memory "$MEMORY" --cpus "$CPUS" \
	--exec "SUITES='$spec' EXAMPLES='$exspec' KORU_ITERS='$KORU_ITERS' \
		KORU_SEED='$KORU_SEED' sh scripts/rust.sh $*" 2>&1)

verdict=$(echo "$out" | grep -oE 'KORU-RUST-(PASS|FAIL)' | tail -1)
case "$verdict" in
KORU-RUST-PASS)
	echo "$out" | grep -E '^(=== suite|=== example|test result:|example )'
	echo "KORU-RUST-PASS"
	exit 0
	;;
*)
	echo "${verdict:-NO VERDICT (guest died or hung?)}"
	echo "--- last section reached ---"
	echo "$out" | grep -E '^=== ' | tail -3
	echo "--- failures ---"
	echo "$out" | grep -E -e "^(test .* FAILED|failures:|thread '|assertion)" \
		-e 'NO KASAN|NO /proc|NO kmemleak|TOO FEW|SKIPPED|UNEXPECTED|NOT BUILT' | head -30
	echo "--- tail ---"
	echo "$out" | tail -30
	exit 1
	;;
esac
