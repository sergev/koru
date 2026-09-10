// SPDX-License-Identifier: GPL-2.0
//
// T6 done test: teardown with work in flight, and module pinning.
//
// Modes:
//   (none)   in-process checks
//   hold     keep an fd open, print READY, sleep; the script tries rmmod
//   pending  submit delays and exit at once, leaving work queued

#include "xring_test.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SQ_ENTRIES 64
#define CQ_ENTRIES 128
#define DELAY_NS (5000 * MS)

/* Submit n delays without waiting for them. */
static int submit_delays(int fd, unsigned n, uint64_t ns)
{
	struct xring_sqe sq[8];
	struct xring_enter e;
	unsigned i;

	for (i = 0; i < n; i++)
		sqe_delay(&sq[i], 0x7000 + i, ns);
	enter_init(&e, sq, n, NULL, 0);
	return ioctl(fd, XRING_IOC_ENTER, &e);
}

/* Block in ENTER on a long delay, then die however the parent decides. */
static void child_blocks_forever(void)
{
	struct xring_sqe s;
	struct xring_cqe c;
	struct xring_enter e;
	int fd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);

	if (fd < 0)
		_exit(2);
	/* Long enough to block, short enough to drain. */
	sqe_delay(&s, 0x8000, 8000 * MS);
	enter_init(&e, &s, 1, &c, 1);
	e.min_complete = 1;
	printf("R\n");
	fflush(stdout);
	ioctl(fd, XRING_IOC_ENTER, &e);
	_exit(0);
}

int main(int argc, char **argv)
{
	int fd, ret, status;
	pid_t pid;

	test_begin(120);

	if (argc > 1 && strcmp(argv[1], "hold") == 0) {
		fd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
		if (fd < 0)
			return 1;
		printf("READY\n");
		sleep(3);
		close(fd);
		return 0;
	}

	if (argc > 1 && strcmp(argv[1], "pending") == 0) {
		fd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
		if (fd < 0)
			return 1;
		if (submit_delays(fd, 4, DELAY_NS) != 4)
			return 1;
		printf("READY\n");
		/* Exit at once: the fd closes, the four delays stay queued. */
		return 0;
	}

	/* 1. close(fd) with four 5 s delays in flight. */
	fd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
	if (fd < 0)
		return 1;
	ret = submit_delays(fd, 4, DELAY_NS);
	check(ret == 4, "4 x DELAY_NS(5s) submitted");
	check(close(fd) == 0, "close(fd) with them in flight succeeds");

	/* 2. SIGKILL a task blocked in ENTER. */
	pid = fork();
	if (pid == 0)
		child_blocks_forever();
	usleep(300000);
	kill(pid, SIGKILL);
	status = 0;
	waitpid(pid, &status, 0);
	check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
	      "SIGKILL on a task blocked in ENTER kills it");

	/* 3. The blocked task's fd closes underneath it as the process dies. */
	pid = fork();
	if (pid == 0)
		child_blocks_forever();
	usleep(300000);
	kill(pid, SIGKILL);
	status = 0;
	waitpid(pid, &status, 0);
	check(WIFSIGNALED(status), "again, with its ring torn down by exit");

	/* 4. A ring closed with work in flight still accepts a fresh one. */
	fd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
	check(fd >= 0, "a new ring opens while old work is still queued");
	if (fd >= 0) {
		ret = submit_delays(fd, 1, MS);
		check(ret == 1, "  and accepts a submission");
		close(fd);
	}

	return test_end();
}
