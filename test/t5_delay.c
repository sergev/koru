// SPDX-License-Identifier: GPL-2.0
//
// T5 done test: DELAY_NS, blocking wait, admission control.
// Structs mirror kernel/xring_abi.rs by hand until T16.

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
_Static_assert(sizeof(struct xring_cqe) == 32, "cqe size");
_Static_assert(sizeof(struct xring_enter) == 64, "enter size");
_Static_assert(offsetof(struct xring_enter, submitted) == 44, "enter.submitted offset");

#define XRING_IOC_SETUP _IOWR('x', 0x00, struct xring_params)
#define XRING_IOC_ENTER _IOWR('x', 0x02, struct xring_enter)

#define XRING_OP_NOP 0
#define XRING_OP_DELAY_NS 1

#define SQ_ENTRIES 64
#define CQ_ENTRIES 128
#define MS 1000000ull

static int failures;

static void check(int ok, const char *what)
{
	printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
	if (!ok)
		failures++;
}

static void check_errno(int ret, int want, const char *what)
{
	if (ret >= 0) {
		printf("%-58s FAIL (succeeded, expected %s)\n", what, strerror(want));
		failures++;
	} else if (errno != want) {
		printf("%-58s FAIL (got %s, expected %s)\n", what, strerror(errno),
		       strerror(want));
		failures++;
	} else {
		printf("%-58s PASS\n", what);
	}
}

static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int open_ring(void)
{
	struct xring_params p;
	int fd = open(XRING_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " XRING_DEV);
		failures++;
		return -1;
	}
	memset(&p, 0, sizeof(p));
	p.magic = XRING_MAGIC;
	p.abi_version = XRING_ABI_VERSION;
	p.sq_entries = SQ_ENTRIES;
	p.cq_entries = CQ_ENTRIES;
	p.slot_size = 4096;
	p.slot_count = 32;
	if (ioctl(fd, XRING_IOC_SETUP, &p) != 0) {
		perror("SETUP");
		failures++;
		close(fd);
		return -1;
	}
	return fd;
}

static void enter_init(struct xring_enter *e, struct xring_sqe *sq, unsigned n,
		       struct xring_cqe *cq, unsigned space)
{
	memset(e, 0, sizeof(*e));
	e->sq_addr = (uint64_t)(uintptr_t)sq;
	e->to_submit = n;
	e->cq_addr = (uint64_t)(uintptr_t)cq;
	e->cq_space = space;
}

static void delay(struct xring_sqe *s, uint64_t user_data, uint64_t ns)
{
	memset(s, 0, sizeof(*s));
	s->opcode = XRING_OP_DELAY_NS;
	s->off = ns;
	s->user_data = user_data;
}

/* A hang is a failure, not a wedged test run. */
static void alarm_die(int sig)
{
	(void)sig;
	_exit(99);
}

static void arm_alarm(unsigned secs)
{
	signal(SIGALRM, alarm_die);
	alarm(secs);
}

static void sigint_noop(int sig)
{
	(void)sig;
}

int main(void)
{
	struct xring_sqe sq[16];
	struct xring_cqe cq[16];
	struct xring_enter e;
	uint64_t t0, dt;
	int fd, ret;
	unsigned i;

	/* Line-buffered: the alarm handler _exit()s, which would drop a full buffer
	 * and leave a hang with no output saying where. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	arm_alarm(60);

	/* 1. Four 50 ms delays, min_complete 4: concurrent, not serial. */
	fd = open_ring();
	if (fd < 0)
		return 1;
	for (i = 0; i < 4; i++)
		delay(&sq[i], 0x1000 + i, 50 * MS);
	enter_init(&e, sq, 4, cq, 16);
	e.min_complete = 4;
	t0 = now_ms();
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	dt = now_ms() - t0;
	printf("  (four 50 ms delays took %llu ms)\n", (unsigned long long)dt);
	check(ret == 4 && e.submitted == 4, "4 x DELAY_NS(50ms): 4 consumed");
	check(e.completed == 4, "  min_complete 4 satisfied");
	check(dt >= 40, "  waited for them (>= 40 ms)");
	check(dt < 150, "  ran concurrently (< 150 ms, not ~200)");
	int ok = 1;
	for (i = 0; i < 4; i++)
		if (cq[i].res != 0)
			ok = 0;
	check(ok, "  each completes res == 0");
	ok = 1;
	for (i = 0; i < 4; i++) {
		unsigned j, seen = 0;
		for (j = 0; j < 4; j++)
			if (cq[j].user_data == 0x1000 + i)
				seen = 1;
		if (!seen)
			ok = 0;
	}
	check(ok, "  every user_data comes back");

	/* 2. A timeout shorter than the delay returns nothing. */
	delay(&sq[0], 0x2000, 1000 * MS);
	enter_init(&e, sq, 1, cq, 16);
	e.min_complete = 1;
	e.timeout_ns = 10 * MS;
	t0 = now_ms();
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	dt = now_ms() - t0;
	check(ret == 1 && e.completed == 0, "10 ms timeout vs 1 s delay: 0 completions");
	check(dt < 500, "  returned promptly, did not wait out the delay");

	/* 3. min_complete 0 does not wait, even with work in flight. */
	enter_init(&e, NULL, 0, cq, 16);
	t0 = now_ms();
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	dt = now_ms() - t0;
	check(ret == 0 && dt < 200, "min_complete 0 returns immediately");
	close(fd); /* abandons the 1 s delay; teardown is T6 */

	/* 4. The idle-ring deadlock foot-gun. */
	fd = open_ring();
	if (fd < 0)
		return 1;
	enter_init(&e, NULL, 0, cq, 16);
	e.min_complete = 1;
	t0 = now_ms();
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	dt = now_ms() - t0;
	check(ret == 0 && e.completed == 0 && dt < 200,
	      "ENTER(0, min_complete 1) on an idle ring returns at once");

	/* 5. DELAY_NS reads only off. */
	delay(&sq[0], 0x3000, MS);
	sq[0].len = 1;
	enter_init(&e, sq, 1, cq, 16);
	e.min_complete = 1;
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "DELAY_NS with non-zero len yields res == -EINVAL");

	delay(&sq[0], 0x3001, MS);
	sq[0].handle = 9;
	enter_init(&e, sq, 1, cq, 16);
	e.min_complete = 1;
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "DELAY_NS with non-zero handle yields res == -EINVAL");

	/* 6. Admission control counts in-flight work, not just queued CQEs. */
	unsigned total = 0;
	for (i = 0; i < CQ_ENTRIES / 8 + 2; i++) {
		unsigned j;
		for (j = 0; j < 8; j++)
			delay(&sq[j], 0x4000 + total + j, 2000 * MS);
		enter_init(&e, sq, 8, cq, 0);
		ret = ioctl(fd, XRING_IOC_ENTER, &e);
		if (ret < 0) {
			perror("ENTER");
			failures++;
			break;
		}
		total += ret;
		if (ret < 8)
			break;
	}
	check(total == CQ_ENTRIES, "in-flight ops count against cq_entries");
	close(fd);

	/* 7. A signal interrupts the wait, and the count survives. */
	int pipefd[2];
	if (pipe(pipefd) != 0) {
		perror("pipe");
		return 1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		struct xring_sqe s;
		struct xring_cqe c;
		struct xring_enter en;
		int r, cfd = open_ring();
		signal(SIGINT, sigint_noop);
		if (cfd < 0)
			_exit(2);
		delay(&s, 0x5000, 30ull * 1000 * MS);
		enter_init(&en, &s, 1, &c, 1);
		en.min_complete = 1;
		write(pipefd[1], "x", 1);
		r = ioctl(cfd, XRING_IOC_ENTER, &en);
		if (r >= 0)
			_exit(3); /* should have been interrupted */
		if (errno != EINTR)
			_exit(4);
		if (en.submitted != 1)
			_exit(5); /* the count must survive EINTR */
		_exit(0);
	}
	char b;
	read(pipefd[0], &b, 1);
	usleep(200000);
	kill(pid, SIGINT);
	int status = 0;
	waitpid(pid, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "SIGINT during ENTER returns EINTR with submitted intact");
	if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
		printf("    (child exit code %d)\n", WEXITSTATUS(status));

	/* 8. SIGKILL must actually kill: no unkillable D state. */
	pid = fork();
	if (pid == 0) {
		struct xring_sqe s;
		struct xring_cqe c;
		struct xring_enter en;
		int cfd = open_ring();
		if (cfd < 0)
			_exit(2);
		delay(&s, 0x6000, 60ull * 1000 * MS);
		enter_init(&en, &s, 1, &c, 1);
		en.min_complete = 1;
		write(pipefd[1], "x", 1);
		ioctl(cfd, XRING_IOC_ENTER, &en);
		_exit(0);
	}
	read(pipefd[0], &b, 1);
	usleep(200000);
	kill(pid, SIGKILL);
	status = 0;
	waitpid(pid, &status, 0);
	check(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
	      "SIGKILL during ENTER kills the task");
	close(pipefd[0]);
	close(pipefd[1]);

	(void)check_errno;
	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
