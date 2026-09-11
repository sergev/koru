#!/bin/sh
# T9 done test, run inside the virtme-ng guest.
# Done test: OPEN/CLOSE, the generational handle table, no fd leak, and an
# unprivileged submitter getting EACCES on /etc/shadow.

. "$(dirname "$0")/common.sh"

dt_begin "T9" "OPEN/CLOSE, handles, creds"
dt_insmod
dt_run "test/t9_handles"
dt_finish
