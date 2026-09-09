// SPDX-License-Identifier: GPL-2.0
//
// T6 done test: teardown with work in flight, and module pinning.
// Structs mirror kernel/xring_abi.rs by hand until T16.
//
// Modes:
//   (none)   in-process checks
//   hold     keep an fd open, print READY, sleep; the script tries rmmod
//   pending  submit delays and exit at once, leaving work queued

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define XRING_DEV "/dev/xring"
#define XRING_MAGIC 0x676e7278u
#define XRING_ABI_VERSION 1u

struct xring_params {
	uint32_t magic, abi_version, flags, sq_entries;
	uint32_t cq_entries, slot_size, slot_count, configured;
	uint64_t features, arena_size;
	uint32_t max_sq_entries, max_cq_entries, max_slot_size, max_slot_count;
	uint64_t max_arena_bytes;
	uint64_t reserved[4];
};

struct xring_sqe {
	uint8_t opcode, flags;
	uint16_t rsvd0;
	uint32_t len;
	uint64_t off;
	uint64_t user_data;
	uint32_t slot, handle;
};

struct xring_cqe {
	uint64_t user_data;
	int64_t res;
	uint32_t flags, rsvd0;
	uint64_t extra;
};

struct xring_enter {
	uint64_t sq_addr, cq_addr, timeout_ns;
	uint32_t to_submit, cq_space, min_complete, flags;
	uint32_t completed, submitted;
	uint64_t reserved[2];
};

_Static_assert(sizeof(struct xring_sqe) == 32, "sqe size");
_Static_assert(sizeof(struct xring_enter) == 64, "enter size");

#define XRING_IOC_SETUP _IOWR('x', 0x00, struct xring_params)
#define XRING_IOC_ENTER _IOWR('x', 0x02, struct xring_enter)

#define XRING_OP_DELAY_NS 1
#define MS 1000000ull
#define DELAY_NS (5000 * MS)

static int failures;

static void check(int ok, const char *what)
{
	printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
	if (!ok)
		failures++;
}

static int open_ring(void)
{
	struct xring_params p;
	int fd = open(XRING_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " XRING_DEV);
		return -1;
	}
	memset(&p, 0, sizeof(p));
	p.magic = XRING_MAGIC;
	p.abi_version = XRING_ABI_VERSION;
	p.sq_entries = 64;
	p.cq_entries = 128;
	p.slot_size = 4096;
	p.slot_count = 32;
	if (ioctl(fd, XRING_IOC_SETUP, &p) != 0) {
		perror("SETUP");
		close(fd);
		return -1;
	}
	return fd;
}

/* Submit n delays without waiting for them. */
static int submit_delays(int fd, unsigned n, uint64_t ns)
{
	struct xring_sqe sq[8];
	struct xring_enter e;
	unsigned i;

	for (i = 0; i < n; i++) {
		memset(&sq[i], 0, sizeof(sq[i]));
		sq[i].opcode = XRING_OP_DELAY_NS;
		sq[i].off = ns;
		sq[i].user_data = 0x7000 + i;
	}
	memset(&e, 0, sizeof(e));
	e.sq_addr = (uint64_t)(uintptr_t)sq;
	e.to_submit = n;
	return ioctl(fd, XRING_IOC_ENTER, &e);
}

static void alarm_die(int sig)
{
	(void)sig;
	_exit(99);
}

/* Block in ENTER on a long delay, then die however the parent decides. */
static void child_blocks_forever(void)
{
	struct xring_sqe s;
	struct xring_cqe c;
	struct xring_enter e;
	int fd = open_ring();

	if (fd < 0)
		_exit(2);
	memset(&s, 0, sizeof(s));
	s.opcode = XRING_OP_DELAY_NS;
	s.off = 8000 * MS; /* long enough to block, short enough to drain */
	memset(&e, 0, sizeof(e));
	e.sq_addr = (uint64_t)(uintptr_t)&s;
	e.to_submit = 1;
	e.cq_addr = (uint64_t)(uintptr_t)&c;
	e.cq_space = 1;
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

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGALRM, alarm_die);
	alarm(120);

	if (argc > 1 && strcmp(argv[1], "hold") == 0) {
		fd = open_ring();
		if (fd < 0)
			return 1;
		printf("READY\n");
		sleep(3);
		close(fd);
		return 0;
	}

	if (argc > 1 && strcmp(argv[1], "pending") == 0) {
		fd = open_ring();
		if (fd < 0)
			return 1;
		if (submit_delays(fd, 4, DELAY_NS) != 4)
			return 1;
		printf("READY\n");
		/* Exit at once: the fd closes, the four delays stay queued. */
		return 0;
	}

	/* 1. close(fd) with four 5 s delays in flight. */
	fd = open_ring();
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
	fd = open_ring();
	check(fd >= 0, "a new ring opens while old work is still queued");
	if (fd >= 0) {
		ret = submit_delays(fd, 1, MS);
		check(ret == 1, "  and accepts a submission");
		close(fd);
	}

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
