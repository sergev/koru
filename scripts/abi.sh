#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The ABI conformance gate. Two pairs, one comparison:
#
#   koru    cpp/include/koru_abi.h and koru_errno.h against
#           rust/sys/src/abi.rs and error.rs, through the abi_dump binaries.
#   screen  screen/ks_abi.h against rust/runtime/src/ks_abi.rs, through the
#           ks_dump binaries. The header is canonical: the daemon is the server.
#
# No VM, no device, no module. CMake registers this same script as the
# `abi_conformance` ctest, so there is one implementation of the comparison.
#
#   scripts/abi.sh
#   scripts/abi.sh --cpp build-asan/abi_dump --ks build-asan/ks_dump
#
# It does not build. Do that first:
#   cmake -B build && cmake --build build
#
# A bare `diff <(a) <(b)` would pass when both sides print nothing, which is
# the same hole WANT_PASSED closes in rust.sh. So both exit statuses, both
# lengths and a minimum record count all gate the verdict.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CPP=$ROOT/build/abi_dump
KS=$ROOT/build/ks_dump
# Raise when a surface grows.
WANT_RECORDS=${WANT_RECORDS:-202}
WANT_KS_RECORDS=${WANT_KS_RECORDS:-177}

while [ $# -gt 0 ]; do
	case $1 in
	--cpp)
		CPP=$2
		shift 2
		;;
	--ks)
		KS=$2
		shift 2
		;;
	*)
		echo "usage: $0 [--cpp path/to/abi_dump] [--ks path/to/ks_dump]" >&2
		exit 2
		;;
	esac
done

# rustup puts cargo here and ctest does not always inherit a login shell.
CARGO=$(command -v cargo 2>/dev/null || echo "$HOME/.cargo/bin/cargo")

fail=0

verdict() {
	echo
	echo "=== KORU-ABI-$1 ==="
	[ "$1" = PASS ] || exit 1
	exit 0
}

if [ ! -x "$CARGO" ]; then
	echo "no cargo on PATH and none at $CARGO"
	verdict FAIL
fi

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# One pair: the C++ binary, the cargo bin that mirrors it, and the floor.
compare() {
	label=$1
	cpp_bin=$2
	rust_bin=$3
	want=$4

	echo
	echo "=== $label ==="
	if [ ! -x "$cpp_bin" ]; then
		echo "no C++ dump at $cpp_bin"
		echo "run: cmake -B build && cmake --build build"
		fail=1
		return
	fi

	"$cpp_bin" >"$tmp/$label.cpp" 2>"$tmp/$label.cpp.err"
	cpp_rc=$?
	"$CARGO" run -q --manifest-path "$ROOT/rust/Cargo.toml" --bin "$rust_bin" \
		>"$tmp/$label.rust" 2>"$tmp/$label.rust.err"
	rust_rc=$?

	bad=0
	for side in cpp rust; do
		eval rc=\$${side}_rc
		n=$(wc -l <"$tmp/$label.$side")
		echo "$side: exit $rc, $n records"
		if [ "$rc" -ne 0 ]; then
			echo "DUMP FAILED"
			head -20 "$tmp/$label.$side.err"
			bad=1
		elif [ "$n" -lt "$want" ]; then
			echo "TOO FEW RECORDS (want at least $want)"
			bad=1
		fi
	done

	if [ "$bad" -eq 0 ]; then
		if diff -u "$tmp/$label.cpp" "$tmp/$label.rust" >"$tmp/$label.diff"; then
			echo "the two dumps agree"
		else
			echo "THE DUMPS DIFFER (- C++, + Rust)"
			head -40 "$tmp/$label.diff"
			bad=1
		fi
	fi
	[ "$bad" -eq 0 ] || fail=1
}

compare koru "$CPP" abi_dump "$WANT_RECORDS"
compare screen "$KS" ks_dump "$WANT_KS_RECORDS"

[ "$fail" -eq 0 ] && verdict PASS
verdict FAIL
