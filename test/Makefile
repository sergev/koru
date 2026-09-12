# SPDX-License-Identifier: MIT
#
# koru_check: the integrated test. Built on the host, run inside the virtme-ng
# guest, where this directory is visible at the same path.
#
# The binding test suites arrive later: Rust at T13, C++ at T39.

CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -pthread
LDLIBS := -pthread

SRCS := koru_check.c koru_ops.c koru_race.c koru_fuzz.c koru_test.c
OBJS := $(SRCS:.c=.o)
DEPS := koru_abi.h koru_test.h koru_check.h

all: koru_check

koru_check: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): %.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f koru_check $(OBJS)

.PHONY: all clean
