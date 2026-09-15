#!/bin/sh
# SPDX-License-Identifier: MIT
#
# The guest side of the screen's end-to-end checks: auto-spawn, the lifecycle,
# and `less` painting a real window. Runs inside the virtme-ng guest, never on
# the host, and prints exactly one KORU-E2E-PASS or KORU-E2E-FAIL marker.
#
# Everything here needs three things the other gates do not: the module (the
# clients are koru programs), SDL's offscreen driver (the daemon owns a window)
# and a writable runtime directory (the socket).
#
# PROBE, LESS, HELLO, DATE and DAEMON are absolute paths, passed in by
# scripts/run-e2e.sh.

ROOT=$(cd "$(dirname "$0")/.." && pwd)
KO=${KO:-$ROOT/kernel/koru.ko}
PROBE=${PROBE:-}
LESS=${LESS:-}
HELLO=${HELLO:-}
DATE=${DATE:-}
DAEMON=${DAEMON:-$ROOT/build/koru-screen}
CPPLESS=${CPPLESS:-$ROOT/build/cmd_less}
CPPEDIT=${CPPEDIT:-$ROOT/build/cmd_edit}
PIXEL=${PIXEL:-$ROOT/build/ks_pixel}
RUN=/tmp/koru-e2e
SOCK=$RUN/koru-screen.sock

fail=0
fence() { echo; echo "=== $1 ==="; }

want_eq() {
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH $1: got [$2] want [$3]"
		fail=1
	fi
}

want() {
	if [ "$2" -eq 0 ]; then
		echo "ok: $1"
	else
		echo "FAILED: $1"
		fail=1
	fi
}

# The daemon by its exact command line, never by a name: this script's own
# environment carries the daemon's path, so `pgrep -f koru-screen` counts the
# shell running the script as well and every count is one too many.
daemons() {
	pgrep -c -x -f "$DAEMON" 2>/dev/null || echo 0
}

daemon_pid() {
	pgrep -x -f "$DAEMON" 2>/dev/null | head -1
}

reset_run() {
	pkill -x -f "$DAEMON" 2>/dev/null
	sleep 0.2
	rm -rf $RUN
	mkdir -p $RUN
	chmod 700 $RUN
}

export SDL_VIDEODRIVER=offscreen
export KORU_SCREEN_SOCK=$SOCK
export KORU_SCREEN_BIN=$DAEMON
export XDG_RUNTIME_DIR=$RUN

echo "=== koru screen end to end ==="
uname -r

fence "environment"
for f in "$PROBE" "$LESS" "$HELLO" "$DATE" "$DAEMON" "$CPPLESS" "$CPPEDIT"; do
	if [ ! -x "$f" ]; then
		echo "NOT BUILT: $f"
		fail=1
	fi
done
[ "$fail" -eq 0 ] || { echo; echo "=== KORU-E2E-FAIL ==="; exit 1; }

insmod "$KO" || { echo "INSMOD FAILED"; echo; echo "=== KORU-E2E-FAIL ==="; exit 1; }

# --------------------------------------------------------------- the spawn race
#
# Twenty clients at once against no daemon. Exactly one daemon must exist
# afterwards and all twenty must connect; without the flock, more than one
# appears and the losers' sockets are unlinked under the winner.
fence "spawn race"
reset_run
# One file per client, never one shared: twenty processes appending to one
# file braid their lines together, and the count comes out short for a reason
# that has nothing to do with the spawn. It is the single-writer rule again,
# in the harness this time.
i=0
while [ $i -lt 20 ]; do
	"$PROBE" connect >/dev/null 2>$RUN/race.$i.err &
	i=$((i + 1))
done
wait
connected=$(grep -l '^connected ' $RUN/race.*.err 2>/dev/null | wc -l)
want_eq "twenty clients connected" "$connected" "20"
want_eq "exactly one daemon" "$(daemons)" "1"
[ -S "$SOCK" ] && echo "ok: the socket is where the clients found it" || {
	echo "FAILED: no socket"
	fail=1
}

# --------------------------------------------------- a live daemon is not stale
#
# The same twenty against a daemon that is already running: its pid must not
# change, which is the rule "only the lock holder may unlink" protecting a
# socket that is alive.
fence "a live daemon survives twenty clients"
was=$(daemon_pid)
i=0
while [ $i -lt 20 ]; do
	"$PROBE" connect >/dev/null 2>$RUN/live.$i.err &
	i=$((i + 1))
done
wait
want_eq "twenty more connected" "$(grep -l '^connected ' $RUN/live.*.err 2>/dev/null | wc -l)" "20"
want_eq "the daemon is the same one" "$(daemon_pid)" "$was"
want_eq "and there is still one" "$(daemons)" "1"

# ------------------------------------------------------------- a stale socket
#
# Kill the daemon without letting it clean up. The socket file is still there
# and nothing is listening: the next client must unlink it and start one.
fence "a stale socket is replaced"
kill -9 "$was" 2>/dev/null
sleep 0.3
[ -S "$SOCK" ] && echo "ok: the socket outlived the daemon" || {
	echo "FAILED: the socket went with it, so this case tests nothing"
	fail=1
}
"$PROBE" connect >/dev/null 2>$RUN/stale.err
want "a client replaced the stale socket" $?
want_eq "one daemon again" "$(daemons)" "1"
[ "$(daemon_pid)" != "$was" ] && echo "ok: a new daemon" || {
	echo "FAILED: the old pid is still there"
	fail=1
}

# ------------------------------------------------------------- daemon death
#
# A client holding the screen when the daemon is killed must report a dead
# connection and leave, not hang. Exit status 2 is the probe's word for it.
fence "a client survives the daemon"
"$PROBE" hold 4000 >/dev/null 2>$RUN/dead.err &
client=$!
sleep 0.6
kill -9 "$(daemon_pid)" 2>/dev/null
wait $client
rc=$?
want_eq "the client exited non-zero" "$rc" "2"
grep -q "closed" $RUN/dead.err && echo "ok: it said the connection was closed" || {
	echo "FAILED: what it said:"
	cat $RUN/dead.err
	fail=1
}

# -------------------------------------------------------------- client death
#
# Kill a client mid-blit. The daemon must take its claims back, serve the next
# client, and log no protocol error.
fence "the daemon survives a client"
reset_run
"$PROBE" hold 5000 >/dev/null 2>$RUN/held.err &
victim=$!
sleep 0.8
want_eq "the victim is holding the screen" "$(grep -c '^connected ' $RUN/held.err)" "1"
kill -9 $victim 2>/dev/null
wait $victim 2>/dev/null
sleep 0.3
"$PROBE" connect >/dev/null 2>$RUN/next.err
want "the next client was served" $?
want_eq "the daemon is still there" "$(daemons)" "1"
want_eq "and it logged nothing" "$(cat $RUN/daemon.log 2>/dev/null | wc -l)" "0"

# ---------------------------------------------------------- the byte channel
#
# T38b. A koru program's stdout is the daemon's parser when — and only when —
# stdout is the terminal koru was started from and a daemon is already there.
# `script` supplies that terminal, so what the pty saw is exactly what would
# have been printed where koru was started.
fence "the byte channel"
reset_run
export KORU_SCREEN_SNAP=$RUN/bc.bmp
# The daemon first, and by a program that asked for a screen: `install`
# connects and never spawns, so a program that wanted no window gets none.
"$PROBE" connect >/dev/null 2>$RUN/bc0.err
want_eq "a daemon to print on" "$(daemons)" "1"

script -qec "$HELLO" /dev/null >$RUN/hello.tty 2>&1
sleep 0.3
want_eq "hello printed nothing where koru was started" "$(wc -c <$RUN/hello.tty)" "0"

if [ -f "$RUN/bc.bmp" ] && [ -x "$PIXEL" ]; then
	size=$("$PIXEL" $RUN/bc.bmp size)
	cols=${size%x*}
	# The cell is 16x20, as the pager's case works it out. Every row is
	# measured from its second cell, because the first is where the cursor
	# sits: a block cursor is 320 pixels of ink and would make a blank row
	# look written on.
	row_ink() {
		"$PIXEL" $RUN/bc.bmp ink 16 $(($1 * 20)) $((cols - 16)) 20
	}
	want_ink() {
		if [ "$(row_ink "$2")" -gt 0 ]; then
			echo "ok: $1"
		else
			echo "FAILED: $1"
			fail=1
		fi
	}

	want_ink "it printed on the scrolling screen instead" 0

	script -qec "$DATE" /dev/null >$RUN/date.tty 2>&1
	sleep 0.3
	want_eq "date printed nothing there either" "$(wc -c <$RUN/date.tty)" "0"
	want_ink "and its line is the screen's second" 1

	# The conditional half. A redirected stdout is the user's own instruction:
	# it stays where it points, and the screen does not move.
	"$HELLO" >$RUN/hello.file 2>$RUN/hello.file.err
	sleep 0.3
	want_eq "a redirected stdout is unaffected" "$(cat $RUN/hello.file)" "Hello, world!"
	want_eq "and nothing of it reached the screen" "$(row_ink 2)" "0"

	# The ordering rule. A program that paints *and* prints: its bytes belong
	# to the scrolling screen it was writing to, so they must not appear in
	# the blit, and they must appear when the claim goes back.
	script -qec "$PROBE print 1500" /dev/null >$RUN/print.tty 2>&1 &
	printer=$!
	sleep 0.8
	cp $RUN/bc.bmp $RUN/held.bmp
	# Eight cells of the banner. A braided print would land at the alternate
	# screen's home, which is exactly there, and paint them black.
	want_eq "the banner is whole while the screen is held" \
		"$("$PIXEL" $RUN/held.bmp modal 0 0 128 20)" "cd00cd"
	want_eq "the print did not go to the terminal either" \
		"$(grep -c PRINTED $RUN/print.tty)" "0"
	wait $printer
	sleep 0.5
	want_ink "the print arrived when the screen came back" 2

	# The byte channel is a socket, and a socket is not a character device:
	# only the runtime knows it is the console. Without that, a buffered
	# stream is fully buffered and this line waits for the at-exit flush.
	script -qec "$PROBE buffered 2000" /dev/null >$RUN/buf.tty 2>&1 &
	buffered=$!
	sleep 0.8
	want_ink "a buffered line is out before the program is" 3
	wait $buffered
else
	echo "FAILED: no snapshot from the daemon"
	fail=1
fi
unset KORU_SCREEN_SNAP

# ----------------------------------------------------------------- less
#
# The deliverable: Braam's pager, painting a real window through the daemon.
# The keys are pushed in by the daemon's own event path, so this asserts that
# `less` ran and repainted, and the pixels are T35's business.
fence "less"
reset_run
seq 1 200 | sed 's/^/line /' >$RUN/fixture.txt
KORU_SCREEN_SNAP=$RUN/less.bmp "$LESS" $RUN/fixture.txt >/dev/null 2>$RUN/less.err &
pager=$!
sleep 1.5
if kill -0 $pager 2>/dev/null; then
	echo "ok: less is up and painting"
else
	echo "FAILED: less exited early:"
	cat $RUN/less.err
	fail=1
fi
want_eq "one daemon for the pager" "$(daemons)" "1"

# What it painted, in pixels. The status line is the assertion that matters:
# `less` paints it black on cyan, which nothing else in this run does, and a
# pager that ran but drew nothing would leave it the background colour.
if [ -f "$RUN/less.bmp" ] && [ -x "$PIXEL" ]; then
	size=$("$PIXEL" $RUN/less.bmp size)
	echo "   window $size"
	rows=${size#*x}
	cols=${size%x*}
	# The cell size is the font's, times the daemon's scale of 2: 16x20.
	last=$((rows - 20))
	want_eq "the status line is cyan" "$("$PIXEL" $RUN/less.bmp modal 0 $last "$cols" 20)" "00cdcd"
	body_ink=$("$PIXEL" $RUN/less.bmp ink 0 0 "$cols" 20)
	[ "$body_ink" -gt 0 ] && echo "ok: the first body row has text on it" || {
		echo "FAILED: the body row is blank"
		fail=1
	}
else
	echo "FAILED: no snapshot from the daemon"
	fail=1
fi
kill -9 $pager 2>/dev/null
wait $pager 2>/dev/null

# ------------------------------------------- the same pager in both bindings
#
# T48's done test. Braam's `less`, compiled once against the Rust binding and
# once against the C++ one, painting the same file through the same daemon:
# the two window snapshots must be **byte-identical**.
#
# This is the language-neutrality claim made on a surface that paints rather
# than one that prints, and it is a stronger test than two demos emitting the
# same text: every cell, every colour and the cursor all have to agree, over a
# protocol neither binding shares a line of code for.
fence "less in two bindings"
# The snapshot is copied **while the pager is still alive**. Killing it gives
# the alternate screen back, the daemon repaints the scrolling one, and the
# file then holds a blank window with a cursor in it — which is what the first
# version of this case compared, twice, and called a match.
snap_pager() {
	pkill -x -f "$DAEMON" 2>/dev/null
	sleep 0.3
	rm -f "$SOCK" "$SOCK.lock" "$2" "$3"
	KORU_SCREEN_SNAP="$2" "$1" $RUN/fixture.txt >/dev/null 2>"$3.err" &
	pager=$!
	# Long enough for a daemon started from cold: the window is SDL's, and
	# this is the one place in the run where nothing is warm.
	sleep 4
	if kill -0 $pager 2>/dev/null; then
		cp "$2" "$3" 2>/dev/null
	else
		echo "FAILED: $1 exited early:"
		cat "$3.err"
		fail=1
	fi
	kill -9 $pager 2>/dev/null
	wait $pager 2>/dev/null
}

reset_run
seq 1 200 | sed 's/^/line /' >$RUN/fixture.txt
snap_pager "$LESS" $RUN/live-rs.bmp $RUN/less-rs.bmp
snap_pager "$CPPLESS" $RUN/live-cpp.bmp $RUN/less-cpp.bmp
if [ -f "$RUN/less-rs.bmp" ] && [ -f "$RUN/less-cpp.bmp" ]; then
	# That both painted at all is asserted first: two blank windows would
	# compare equal, and this case would then prove nothing.
	size=$("$PIXEL" $RUN/less-rs.bmp size)
	cols=${size%x*}
	rows=${size#*x}
	last=$((rows - 20))
	echo "   window $size"
	want_eq "the Rust pager painted its status line" \
		"$("$PIXEL" $RUN/less-rs.bmp modal 0 $last "$cols" 20)" "00cdcd"
	if cmp -s $RUN/less-rs.bmp $RUN/less-cpp.bmp; then
		echo "ok: the two bindings painted the same pixels"
	else
		echo "MISMATCH: the two pagers' windows differ"
		cmp $RUN/less-rs.bmp $RUN/less-cpp.bmp | head -3
		fail=1
	fi
else
	echo "FAILED: one of the pagers left no snapshot"
	fail=1
fi
pkill -x -f "$DAEMON" 2>/dev/null

# `less` with no daemon to be had is `cat`, which is the other half of its
# first decision and needs no window at all.
fence "less with no screen"
pkill -x -f "$DAEMON" 2>/dev/null
sleep 0.2
rm -rf $RUN/koru-screen.sock
out=$(KORU_SCREEN_BIN=/nonexistent "$LESS" $RUN/fixture.txt 2>$RUN/cat.err | tail -1)
want_eq "it catted the file instead" "$out" "line 200"
out=$(KORU_SCREEN_BIN=/nonexistent "$CPPLESS" $RUN/fixture.txt 2>$RUN/cat.err | tail -1)
want_eq "and so did the C++ one" "$out" "line 200"

pkill -x -f "$DAEMON" 2>/dev/null

# ------------------------------------------------------------------- edit
#
# T49b's other half: Braam's editor, with the daemon typing at it. The script
# is fed one key at a time and **only while the editor is parked on a key
# read**, so there is no sleep in this case and nothing to lose a race to.
#
# What it types: `Hi` at the start of the first line, Return **in the middle of
# it**, `x`, End, Backspace, Down, Return, `y`, then ^S and ^Q. The file it
# leaves behind is the whole assertion — and the Return has to be mid-line,
# because a split at the end of one cuts nothing and a broken `split` passes.
fence "edit"
reset_run
printf 'alpha\nbravo\n' >$RUN/edit.txt
cat >$RUN/keys <<'KEYS'
H
i
enter
x
end
backspace
down
enter
y
ctrl+s
ctrl+q
KEYS

KORU_SCREEN_KEYS=$RUN/keys "$CPPEDIT" $RUN/edit.txt >/dev/null 2>$RUN/edit.err
want_eq "edit exited cleanly" "$?" "0"
want_eq "edit wrote back what was typed" "$(cat $RUN/edit.txt)" \
	"$(printf 'Hi\nxalph\nbravo\ny')"
# Its temporary file is renamed over the target, so nothing is left beside it.
want_eq "and left no temporary behind" "$(ls $RUN | grep -c '^edit.txt.tmp' || true)" "0"
pkill -x -f "$DAEMON" 2>/dev/null

fence "verdict"
if [ "$fail" -eq 0 ]; then
	echo "=== KORU-E2E-PASS ==="
else
	echo "=== KORU-E2E-FAIL ==="
fi
