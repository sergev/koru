#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The screen gate: the terminal model's suite and the parser's fuzz oracle,
# both under ASan and UBSan. No VM, no device, no module, no SDL — the model
# has zero koru dependencies, which is why it can be checked here.
#
#   scripts/screen.sh
#   KS_SEED=12345 scripts/screen.sh    # replay a fuzz failure
#   KS_ITERS=500000 scripts/screen.sh  # a longer fuzz run
#
# It configures and builds its own directory, because the sanitizers are a
# per-target flag and `build/` is the plain one the ABI diff uses.
#
# clang's sanitizer runtime is a separate Debian package (libclang-rt-*-dev),
# so this defaults to GCC, whose libasan is part of the toolchain. Override
# with CXX=clang++ once that package is installed.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build-asan}
CXX=${CXX:-g++}
CC=${CC:-gcc}
export KS_SEED KS_ITERS

fail=0

verdict() {
	echo
	echo "=== KORU-SCREEN-$1 ==="
	[ "$1" = PASS ] || exit 1
	exit 0
}

cmake -B "$BUILD" -S "$ROOT" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_C_COMPILER="$CC" \
	-DKORU_SANITIZE=ON >/dev/null || verdict FAIL
cmake --build "$BUILD" --target ks_model_test ks_ansi_rand >/dev/null || {
	cmake --build "$BUILD" --target ks_model_test ks_ansi_rand
	verdict FAIL
}

# Braam's own cell-exact assertions, ported. A suite that ran nothing exits 0,
# so the verdict gates on its own "OK: 0 failure(s)" line as well as on the
# status — the same hole the Rust runner's floor closes.
out=$("$BUILD/ks_model_test" 2>&1)
rc=$?
echo "=== model ==="
echo "$out"
[ "$rc" -eq 0 ] || fail=1
echo "$out" | grep -q '^OK: 0 failure(s)$' || {
	echo "NO VERDICT LINE FROM ks_model_test"
	fail=1
}
echo "$out" | grep -q '^== ansi ==$' || {
	echo "THE ANSI CASES DID NOT RUN"
	fail=1
}

echo
echo "=== parser fuzz ==="
out=$("$BUILD/ks_ansi_rand" 2>&1)
rc=$?
echo "$out"
[ "$rc" -eq 0 ] || fail=1
echo "$out" | grep -q '^ks_ansi_rand: OK$' || {
	echo "THE FUZZ DRIVER DID NOT FINISH"
	fail=1
}

[ "$fail" -eq 0 ] && verdict PASS
verdict FAIL
