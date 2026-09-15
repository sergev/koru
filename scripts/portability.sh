#!/bin/sh
# SPDX-License-Identifier: MIT
#
# T49's done test, guest side. Runs inside the virtme-ng guest, never on the
# host: every program here reaches /dev/koru.
#
# Each case runs Braam's program and its coreutils equivalent over two copies
# of one fixture tree and compares four things: stdout byte for byte, the exit
# status, the change the case made to the tree, and whether stderr was written
# to. **Only whether**: a diagnostic here is Braam's "who: what: why" and not
# GNU's wording, and that is a difference in the program, not in the substrate.
#
# A case may name a normaliser, applied to both sides. Four do, and the
# `differences` section at the end asserts those differences directly;
# doc/Notes.md records all six places Braam's programs are not GNU's.
#
# Prints exactly one KORU-PORT-PASS or KORU-PORT-FAIL, which
# scripts/run-portability.sh greps for.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO=${KO:-$ROOT/kernel/koru.ko}
BIN=${BIN:-$ROOT/build}
FLOOR=${FLOOR:-1}
FILTERS="$*"
WORK=/tmp/koru-port
T=$WORK/tmp

fail=0
ran=0

# The three normalisers, each named where it is used and each a difference in
# Braam's output format rather than in what koru carried.
SQUEEZE="sed -e 's/^ *//' -e 's/  */ /g'"
ENDNL="awk 1"
CMPB="sed -e 's/differ: char /differ: byte /'"

fence() { echo "koru-check: $1" >/dev/kmsg 2>/dev/null; echo; echo "=== $1 ==="; }

# The fixture, built identically in each of the two trees.
fixture() {
	d=$1
	mkdir -p "$d/sub/deep" || return 1
	printf 'alpha\nbravo\ncharlie\ndelta\n' >"$d/a.txt"
	printf 'alpha\nbravo\ncharlie\ndelta\n' >"$d/b.txt"
	printf 'alpha\nbravo\nCHARLIE\ndelta\n' >"$d/c.txt"
	printf '1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n' >"$d/nums.txt"
	printf 'one\none\ntwo\nthree\nthree\nthree\nfour\n' >"$d/dup.txt"
	printf 'a,b,c\nd,e,f\nnosep\ng,h,i\n' >"$d/csv.txt"
	printf 'no newline at the end' >"$d/nonl.txt"
	printf 'h\303\251llo w\303\266rld\n' >"$d/utf.txt"
	: >"$d/empty.txt"
	printf 'nested\n' >"$d/sub/one.txt"
	printf 'deeper\n' >"$d/sub/deep/two.txt"
	ln -s a.txt "$d/link"
	dd if=/dev/zero of="$d/big.bin" bs=1024 count=4 2>/dev/null
	printf 'end' >>"$d/big.bin"
}

# What a tree looks like: every name, its type, and a regular file's size. A
# mutating program is judged on this as much as on what it printed.
snapshot() {
	(cd "$1" && find . | sort | while read -r p; do
		if [ -L "$p" ]; then
			echo "$p link $(readlink "$p")"
		elif [ -d "$p" ]; then
			echo "$p dir"
		else
			echo "$p file $(wc -c <"$p")"
		fi
	done)
}

# Whether a case runs at all: no filter, or a filter this name matches.
wanted() {
	[ -z "$FILTERS" ] && return 0
	for f in $FILTERS; do
		case "$1" in
		*"$f"*) return 0 ;;
		esac
	done
	return 1
}

# check <name> <reference command> <koru command> [normaliser]
check() {
	name=$1
	ref=$2
	ours=$3
	norm=${4:-cat}

	wanted "$name" || return 0
	ran=$((ran + 1))

	# Before and after, so what is compared is the change this case made. A
	# tree that has already diverged would otherwise fail every case after it.
	snapshot "$WORK/ref" >"$T/b.r"
	snapshot "$WORK/koru" >"$T/b.k"

	(cd "$WORK/ref" && eval "$ref") >"$T/o.r" 2>"$T/e.r"
	sr=$?
	(cd "$WORK/koru" && eval "$ours") >"$T/o.k" 2>"$T/e.k"
	sk=$?

	eval "$norm" <"$T/o.r" >"$T/n.r"
	eval "$norm" <"$T/o.k" >"$T/n.k"
	snapshot "$WORK/ref" >"$T/a.r"
	snapshot "$WORK/koru" >"$T/a.k"
	diff "$T/b.r" "$T/a.r" >"$T/t.r"
	diff "$T/b.k" "$T/a.k" >"$T/t.k"

	bad=""
	cmp -s "$T/n.r" "$T/n.k" || bad="$bad stdout"
	[ "$sr" = "$sk" ] || bad="$bad status($sr/$sk)"
	if [ -s "$T/e.r" ]; then er=1; else er=0; fi
	if [ -s "$T/e.k" ]; then ek=1; else ek=0; fi
	[ "$er" = "$ek" ] || bad="$bad stderr($er/$ek)"
	cmp -s "$T/t.r" "$T/t.k" || bad="$bad tree"

	if [ -z "$bad" ]; then
		echo "ok: $name"
	else
		echo "FAIL $name:$bad"
		echo "    ref:  $ref"
		echo "    koru: $ours"
		diff "$T/n.r" "$T/n.k" | head -6
		diff "$T/t.r" "$T/t.k" | head -6
		head -2 "$T/e.k"
		fail=1
	fi
}

# The same argument list on each side: `wc` is coreutils', `cmd_wc` is Braam's
# compiled against koru.
both() {
	c=$1
	p=$2
	shift 2
	check "$p-$c" "$p $*" "$BIN/cmd_$p $*"
}

# The same, through a normaliser.
bothn() {
	n=$1
	c=$2
	p=$3
	shift 3
	check "$p-$c" "$p $*" "$BIN/cmd_$p $*" "$n"
}

echo "=== koru portability suite ==="
uname -r

fence "environment"
if ! insmod "$KO" 2>/dev/null; then
	echo "insmod failed"
	echo "KORU-PORT-FAIL"
	exit 1
fi
[ -c /dev/koru ] || {
	echo "no /dev/koru"
	echo "KORU-PORT-FAIL"
	exit 1
}
echo "ok: module loaded"

for p in echo basename dirname pwd seq sleep touch mkdir rm ln truncate \
	cat wc head tail tr cut uniq cmp grep; do
	[ -x "$BIN/cmd_$p" ] || {
		echo "NOT BUILT $BIN/cmd_$p"
		echo "KORU-PORT-FAIL"
		exit 1
	}
done

rm -rf "$WORK"
mkdir -p "$T" "$WORK/ref" "$WORK/koru" || exit 1
fixture "$WORK/ref" || exit 1
fixture "$WORK/koru" || exit 1

# ---------------------------------------------------------------- text, pure

fence "text"
# The shell's `echo` is a builtin and not coreutils', so the reference is named.
check echo-words "/bin/echo a b c" "$BIN/cmd_echo a b c"
check echo-nonl "/bin/echo -n a b" "$BIN/cmd_echo -n a b"
check echo-empty "/bin/echo" "$BIN/cmd_echo"

both plain basename /usr/local/lib/libc.so
both suffix basename /usr/local/lib/libc.so .so
both all basename -a /a/b /c/d e
both strip basename -s .txt a.txt b.txt
both root basename ///

both plain dirname /usr/local/lib/libc.so
both bare dirname nofile
both root dirname /
both runs dirname a//b//

# One directory, named, because the two trees are not the same path.
check pwd "cd $WORK && /bin/pwd" "cd $WORK && $BIN/cmd_pwd"

fence "numbers"
both last seq 5
both range seq 3 9
both step seq 1 3 20
both down seq 10 -2 1
both pad seq -w 8 12
both sep seq -s , 1 5
both fmt seq -f %05.2f 1 0.5 3

# `sleep` prints nothing; the case is that it is quiet and successful.
both zero sleep 0

# --------------------------------------------------------------- filesystem

fence "filesystem"
both new touch made.txt
both again touch a.txt
both many touch m1 m2 m3

both one mkdir d1
both parents mkdir -p p1/p2/p3
both exists mkdir d1
both noparent mkdir nope/child

both file rm m1
both missing rm nothere
both tree rm -r p1
both empty rm -r d1

both symlink ln -s a.txt s1
both force ln -sf b.txt s1
both exists ln -s a.txt s1

both set truncate -s 100 t1
both grow truncate -s +50 t1
both shrink truncate -s -30 t1
both atmost truncate -s '<40' t1
both round truncate -s %512 t1
both ref truncate -r a.txt t2
both nocreate truncate -c -s 10 notthere

# -------------------------------------------------------------------- streams

fence "streams"
both one cat a.txt
both many cat a.txt nums.txt
both nonl cat nonl.txt
both missing cat nosuchfile
both empty cat empty.txt
check cat-stdin "cat < a.txt" "$BIN/cmd_cat < a.txt"

# Braam's `wc` lays its counts out in seven-column fields, as BSD's does; GNU's
# width is the widest count in the run. Squeezing runs of blanks is what makes
# the two comparable, and it is the only normaliser here.
bothn "$SQUEEZE" lines wc -l a.txt
bothn "$SQUEEZE" all wc -lwc a.txt
# GNU's `wc -m` counts bytes in the POSIX locale, so the reference is asked
# for a UTF-8 one; Braam's counts runes whatever the environment says.
check wc-chars "LC_ALL=C.UTF-8 wc -m utf.txt" "$BIN/cmd_wc -m utf.txt" "$SQUEEZE"
bothn "$SQUEEZE" longest wc -L a.txt
bothn "$SQUEEZE" many wc -l a.txt nums.txt
check wc-stdin "wc -l < a.txt" "$BIN/cmd_wc -l < a.txt" "$SQUEEZE"

both default head nums.txt
both lines head -n 3 nums.txt
both bytes head -c 7 a.txt
both many head -n 2 a.txt nums.txt
both quiet head -q -n 2 a.txt nums.txt

both default tail nums.txt
both lines tail -n 3 nums.txt
# Braam's `tail` terminates every line it prints, so a final fragment with no
# newline gains one where GNU's leaves it alone. `awk 1` gives both a trailing
# newline; the section below asserts the difference itself.
bothn "$ENDNL" nonl tail -n 1 nonl.txt

check tr-upper "tr a-z A-Z < a.txt" "$BIN/cmd_tr a-z A-Z < a.txt"
check tr-delete "tr -d aeiou < a.txt" "$BIN/cmd_tr -d aeiou < a.txt"
check tr-squeeze "tr -s l < a.txt" "$BIN/cmd_tr -s l < a.txt"
check tr-class "tr '[:lower:]' '[:upper:]' < a.txt" \
	"$BIN/cmd_tr '[:lower:]' '[:upper:]' < a.txt"
check tr-complement "tr -cd 'a-z\n' < a.txt" "$BIN/cmd_tr -cd 'a-z\n' < a.txt"

both fields cut -d , -f 2 csv.txt
both list cut -d , -f 1,3 csv.txt
both chars cut -c 1-3 a.txt
both bytes cut -b 2- a.txt
both only cut -d , -f 1 -s csv.txt

both plain uniq dup.txt
bothn "$SQUEEZE" count uniq -c dup.txt
both dups uniq -d dup.txt
both unique uniq -u dup.txt
both skip uniq -s 1 dup.txt

both same cmp a.txt b.txt
both differ cmp a.txt c.txt
both silent cmp -s a.txt c.txt
bothn "$SQUEEZE" list cmp -l a.txt c.txt
# Braam's `cmp -b` calls the offset a char where GNU calls it a byte. Only
# that one word is normalised; the rest of the line is compared as it stands.
bothn "$CMPB" bytes cmp -b a.txt c.txt
both limit cmp -n 6 a.txt c.txt
both skip cmp -i 6 a.txt c.txt
both missing cmp a.txt gone.txt

# Braam's `grep` matches fixed text, not a regular expression, so `grep -F` is
# the equivalent and plain `grep` is not.
check grep-plain "grep -F charlie a.txt" "$BIN/cmd_grep charlie a.txt"
check grep-nocase "grep -F -i CHARLIE a.txt" "$BIN/cmd_grep -i CHARLIE a.txt"
check grep-invert "grep -F -v charlie a.txt" "$BIN/cmd_grep -v charlie a.txt"
check grep-nomatch "grep -F zulu a.txt" "$BIN/cmd_grep zulu a.txt"
# Braam's `grep` never prefixes the file name, which is GNU's -h.
check grep-many "grep -F -h a a.txt nums.txt" "$BIN/cmd_grep a a.txt nums.txt"
check grep-stdin "grep -F bravo < a.txt" "$BIN/cmd_grep bravo < a.txt"

# --------------------------------------------------------------- the twins
#
# The second half of T49's done test: a handful of the same programs written as
# idiomatic Rust against the same function names. The reference side here is
# the Rust binary and the koru side is Braam's own C++ source — two bindings
# that share no code, over one unchanged kernel, and the bytes have to agree.

fence "twins"

# twin <case> <program> <args...>: the Rust one against the C++ one.
twin() {
	c=$1
	p=$2
	shift 2
	eval "rs=\$RS_$(echo "$p" | tr '[:lower:]' '[:upper:]')"
	if [ -z "$rs" ] || [ ! -x "$rs" ]; then
		echo "NOT BUILT: no Rust $p example"
		fail=1
		return 0
	fi
	check "twin-$p-$c" "$rs $*" "$BIN/cmd_$p $*"
}

twin words echo a b c
twin nonl echo -n a b
twin empty echo

twin plain basename /usr/local/lib/libc.so
twin suffix basename /usr/local/lib/libc.so .so
twin all basename -a /a/b /c/d e
twin strip basename -s .txt a.txt b.txt
twin badopt basename -z x

twin one cat a.txt
twin many cat a.txt nums.txt
twin nonl cat nonl.txt
twin missing cat nosuchfile
check twin-cat-stdin "$RS_CAT < a.txt" "$BIN/cmd_cat < a.txt"

twin plain grep charlie a.txt
twin nocase grep -i CHARLIE a.txt
twin invert grep -v charlie a.txt
twin nomatch grep zulu a.txt
twin many grep a a.txt nums.txt

twin plain uniq dup.txt
twin count uniq -c dup.txt
twin dups uniq -d dup.txt
twin unique uniq -u dup.txt
twin skip uniq -s 1 dup.txt
twin fields uniq -f 1 csv.txt

# --------------------------------------------- where Braam is not coreutils
#
# Three differences the section above had to normalise around. Each is
# asserted here against what Braam's program does, so the difference is tested
# rather than merely described, and doc/Notes.md records all three.

fence "differences"

# wants <name> <command> <expected stdout, printf %b>
wants() {
	name=$1
	cmd=$2
	want=$3
	wanted "$name" || return 0
	ran=$((ran + 1))
	(cd "$WORK/koru" && eval "$cmd") >"$T/o.k" 2>"$T/e.k"
	printf '%b' "$want" >"$T/w"
	if cmp -s "$T/w" "$T/o.k"; then
		echo "ok: $name"
	else
		echo "FAIL $name"
		diff "$T/w" "$T/o.k" | head -5
		fail=1
	fi
}

wants differs-tail-terminates "$BIN/cmd_tail -n 1 nonl.txt" 'no newline at the end\n'
wants differs-cmp-says-char "$BIN/cmd_cmp -b a.txt c.txt" \
	'a.txt c.txt differ: char 13, line 3 is 143 c 103 C\n'

# `rm` without -r takes an empty directory, where GNU's refuses: koru's
# `remove_path(p, false)` is Braam's, and it is `rmdir` for a directory.
if wanted differs-rm-takes-a-dir; then
	mkdir -p "$WORK/koru/rmtest"
	wants differs-rm-takes-a-dir "$BIN/cmd_rm rmtest && echo gone" 'gone\n'
	[ -d "$WORK/koru/rmtest" ] && {
		echo "FAIL differs-rm-takes-a-dir: it is still there"
		fail=1
	}
	# The reference tree keeps up, so the snapshots stay comparable.
	rm -rf "$WORK/ref/rmtest"
fi

# ------------------------------------------------------------------- verdict

fence "verdict"
echo "cases run: $ran"
if [ "$ran" -lt "$FLOOR" ]; then
	echo "TOO FEW CASES RAN: $ran < $FLOOR"
	fail=1
fi

rmmod koru 2>/dev/null || {
	echo "rmmod failed"
	fail=1
}

splat=$(dmesg | grep -E 'BUG:|WARNING:|Oops|general protection|KASAN|UBSAN|possible circular|not supported for file' |
	grep -v 'koru-check:')
if [ -n "$splat" ]; then
	echo "KERNEL SPLAT:"
	echo "$splat" | head -10
	fail=1
fi

if [ "$fail" = 0 ]; then
	echo "KORU-PORT-PASS"
else
	echo "KORU-PORT-FAIL"
fi
exit "$fail"
