#!/bin/sh
# T12 done test, run inside the virtme-ng guest.
# Done test: ten minutes of hostile userspace under KASAN, lockdep and kmemleak
# with zero kernel messages.
#
# Usage: t12.sh [seconds] [seed]. The fuzzer prints its seed first; passing it
# back replays the same sequence.

. "$(dirname "$0")/common.sh"

dt_begin "T12" "hostile-userspace fuzz"
dt_insmod
dt_run "test/t12_fuzz" "${1:-600}" $2
dt_finish
