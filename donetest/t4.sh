#!/bin/sh
# T4 done test, run inside the virtme-ng guest.
# Done test: 8 NOPs in, 8 CQEs out; a garbage opcode yields a CQE, not an error.

. "$(dirname "$0")/common.sh"

dt_begin "T4" "ENTER, NOP"
dt_insmod
dt_run "test/t4_enter"
dt_finish
