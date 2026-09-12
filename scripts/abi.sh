#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The ABI conformance gate: cpp/include/koru_abi.h and koru_errno.h against
# rust/sys/src/abi.rs and error.rs, through the two abi_dump binaries.
# No VM, no device, no module. CMake registers this same script as the
# `abi_conformance` ctest, so there is one implementation of the comparison.
#
#   scripts/abi.sh
#   scripts/abi.sh --cpp build-asan/abi_dump
#
# It does not build. Do that first:
#   cmake -B build && cmake --build build
#
# A bare `diff <(a) <(b)` would pass when both sides print nothing, which is
# the same hole WANT_PASSED closes in rust.sh. So both exit statuses, both
# lengths and a minimum record count all gate the verdict.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CPP=$ROOT/build/abi_dump
# Raise when the surface grows.
WANT_RECORDS=${WANT_RECORDS:-145}

while [ $# -gt 0 ]; do
	case $1 in
	--cpp)
		CPP=$2
		shift 2
		;;
	*)
		echo "usage: $0 [--cpp path/to/abi_dump]" >&2
		exit 2
		;;
	esac
done

# rustup puts cargo here and ctest does not always inherit a login shell.
CARGO=$(command -v cargo 2>/dev/null || echo "$HOME/.cargo/bin/cargo")

verdict() {
	echo
	echo "=== KORU-ABI-$1 ==="
	[ "$1" = PASS ] || exit 1
	exit 0
}

if [ ! -x "$CPP" ]; then
	echo "no C++ dump at $CPP"
	echo "run: cmake -B build && cmake --build build"
	verdict FAIL
fi
if [ ! -x "$CARGO" ]; then
	echo "no cargo on PATH and none at $CARGO"
	verdict FAIL
fi

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

"$CPP" >"$tmp/cpp" 2>"$tmp/cpp.err"
cpp_rc=$?
"$CARGO" run -q --manifest-path "$ROOT/rust/Cargo.toml" --bin abi_dump \
	>"$tmp/rust" 2>"$tmp/rust.err"
rust_rc=$?

fail=0
for side in cpp rust; do
	eval rc=\$${side}_rc
	n=$(wc -l <"$tmp/$side")
	echo "$side: exit $rc, $n records"
	if [ "$rc" -ne 0 ]; then
		echo "DUMP FAILED"
		head -20 "$tmp/$side.err"
		fail=1
	elif [ "$n" -lt "$WANT_RECORDS" ]; then
		echo "TOO FEW RECORDS (want at least $WANT_RECORDS)"
		fail=1
	fi
done

if [ "$fail" -eq 0 ]; then
	if diff -u "$tmp/cpp" "$tmp/rust" >"$tmp/diff"; then
		echo "the two dumps agree"
	else
		echo "THE DUMPS DIFFER (- C++, + Rust)"
		head -40 "$tmp/diff"
		fail=1
	fi
fi

[ "$fail" -eq 0 ] && verdict PASS
verdict FAIL
