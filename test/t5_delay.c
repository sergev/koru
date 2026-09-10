// SPDX-License-Identifier: GPL-2.0
//
// T5 done test: DELAY_NS, blocking wait, admission control.

#include "koru_test.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SQ_ENTRIES 64
#define CQ_ENTRIES 128

static void sigint_noop(int sig)
{
	(void)sig;
}

int main(void)
{
	struct koru_sqe sq[16];
	struct koru_cqe cq[16];
	struct koru_enter e;
	uint64_t t0, dt;
	int fd, ret;
	unsigned i;

	test_begin(60);

	/* 1. Four 50 ms delays, min_complete 4: concurrent, not serial. */
	fd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
	if (fd < 0)
		return 1;
	for (i = 0; i < 4; i++)
		sqe_delay(&sq[i], 0x1000 + i, 50 * MS);
	enter_init(&e, sq, 4, cq, 16);
	e.min_complete = 4;
	t0 = now_ms();
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
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
	sqe_delay(&sq[0], 0x2000, 1000 * MS);
	enter_init(&e, sq, 1, cq, 16);
	e.min_complete = 1;
	e.timeout_ns = 10 * MS;
	t0 = now_ms();
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	dt = now_ms() - t0;
	check(ret == 1 && e.completed == 0, "10 ms timeout vs 1 s delay: 0 completions");
	check(dt < 500, "  returned promptly, did not wait out the delay");

	/* 3. min_complete 0 does not wait, even with work in flight. */
	enter_init(&e, NULL, 0, cq, 16);
	t0 = now_ms();
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	dt = now_ms() - t0;
	check(ret == 0 && dt < 200, "min_complete 0 returns immediately");
	close(fd); /* abandons the 1 s delay; teardown is T6 */

	/* 4. The idle-ring deadlock foot-gun. */
	fd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
	if (fd < 0)
		return 1;
	enter_init(&e, NULL, 0, cq, 16);
	e.min_complete = 1;
	t0 = now_ms();
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	dt = now_ms() - t0;
	check(ret == 0 && e.completed == 0 && dt < 200,
	      "ENTER(0, min_complete 1) on an idle ring returns at once");

	/* 5. DELAY_NS reads only off. */
	sqe_delay(&sq[0], 0x3000, MS);
	sq[0].len = 1;
	enter_init(&e, sq, 1, cq, 16);
	e.min_complete = 1;
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "DELAY_NS with non-zero len yields res == -EINVAL");

	sqe_delay(&sq[0], 0x3001, MS);
	sq[0].handle = 9;
	enter_init(&e, sq, 1, cq, 16);
	e.min_complete = 1;
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "DELAY_NS with non-zero handle yields res == -EINVAL");

	/* 6. Admission control counts in-flight work, not just queued CQEs. */
	unsigned total = 0;
	for (i = 0; i < CQ_ENTRIES / 8 + 2; i++) {
		unsigned j;
		for (j = 0; j < 8; j++)
			sqe_delay(&sq[j], 0x4000 + total + j, 2000 * MS);
		enter_init(&e, sq, 8, cq, 0);
		ret = ioctl(fd, KORU_IOC_ENTER, &e);
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
		struct koru_sqe s;
		struct koru_cqe c;
		struct koru_enter en;
		int r, cfd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
		signal(SIGINT, sigint_noop);
		if (cfd < 0)
			_exit(2);
		sqe_delay(&s, 0x5000, 30ull * 1000 * MS);
		enter_init(&en, &s, 1, &c, 1);
		en.min_complete = 1;
		write(pipefd[1], "x", 1);
		r = ioctl(cfd, KORU_IOC_ENTER, &en);
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
		struct koru_sqe s;
		struct koru_cqe c;
		struct koru_enter en;
		int cfd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
		if (cfd < 0)
			_exit(2);
		sqe_delay(&s, 0x6000, 60ull * 1000 * MS);
		enter_init(&en, &s, 1, &c, 1);
		en.min_complete = 1;
		write(pipefd[1], "x", 1);
		ioctl(cfd, KORU_IOC_ENTER, &en);
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

	return test_end();
}
