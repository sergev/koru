#!/bin/sh
# T5 done test, run inside the virtme-ng guest.
# Done test: DELAY_NS concurrency, timeout, and interruptible wait.

. "$(dirname "$0")/common.sh"

dt_begin "T5" "DELAY_NS, blocking wait"
dt_insmod
dt_run "test/t5_delay"
dt_finish
