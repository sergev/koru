#!/bin/sh
# T10 done test, run inside the virtme-ng guest.
# Done test: bytes match read(2), a short read at EOF, and a CLOSE racing an
# in-flight READ with no use-after-free under KASAN.

. "$(dirname "$0")/common.sh"

dt_begin "T10" "READ into a slot"
dt_insmod
dt_run "test/t10_read"
dt_finish
