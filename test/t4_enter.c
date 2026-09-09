// SPDX-License-Identifier: GPL-2.0
//
// T4 done test: ENTER, NOP only.
// Structs mirror kernel/xring_abi.rs by hand until T16.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
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
	uint32_t completed, rsvd0;
	uint64_t reserved[2];
};

_Static_assert(sizeof(struct xring_params) == 104, "params size");
_Static_assert(sizeof(struct xring_sqe) == 32, "sqe size");
_Static_assert(sizeof(struct xring_cqe) == 32, "cqe size");
_Static_assert(sizeof(struct xring_enter) == 64, "enter size");
_Static_assert(offsetof(struct xring_sqe, off) == 8, "sqe.off offset");
_Static_assert(offsetof(struct xring_sqe, user_data) == 16, "sqe.user_data offset");
_Static_assert(offsetof(struct xring_cqe, res) == 8, "cqe.res offset");
_Static_assert(offsetof(struct xring_enter, to_submit) == 24, "enter.to_submit offset");

#define XRING_IOC_SETUP _IOWR('x', 0x00, struct xring_params)
#define XRING_IOC_ENTER _IOWR('x', 0x02, struct xring_enter)

#define XRING_OP_NOP 0
#define XRING_OP_DELAY_NS 1

#define SQ_ENTRIES 64
#define CQ_ENTRIES 128

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

static int open_dev(void)
{
	int fd = open(XRING_DEV, O_RDWR);
	if (fd < 0) {
		perror("open " XRING_DEV);
		failures++;
	}
	return fd;
}

/* An open, configured ring. */
static int open_ring(void)
{
	struct xring_params p;
	int fd = open_dev();
	if (fd < 0)
		return -1;
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

static void nop(struct xring_sqe *s, uint64_t user_data)
{
	memset(s, 0, sizeof(*s));
	s->opcode = XRING_OP_NOP;
	s->user_data = user_data;
}

int main(void)
{
	struct xring_sqe sq[16];
	struct xring_cqe cq[16];
	struct xring_enter e;
	int fd, ret;
	unsigned i;

	/* 1. Eight NOPs in, eight CQEs out, user_data matching in order. */
	fd = open_ring();
	if (fd < 0)
		return 1;
	for (i = 0; i < 8; i++)
		nop(&sq[i], 0x1000 + i);
	enter_init(&e, sq, 8, cq, 16);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 8, "ENTER returns 8 SQEs consumed");
	check(e.completed == 8, "  completed == 8");
	int ok = 1, res_ok = 1;
	for (i = 0; i < 8; i++) {
		if (cq[i].user_data != 0x1000 + i)
			ok = 0;
		if (cq[i].res != 0 || cq[i].flags != 0 || cq[i].extra != 0)
			res_ok = 0;
	}
	check(ok, "  user_data matches one for one, in order");
	check(res_ok, "  every NOP completes res == 0, no flags");

	/* 2. A bad SQE is a completion, not an ioctl error (E1). */
	nop(&sq[0], 0x2000);
	sq[0].opcode = 200; /* unknown */
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 1, "unknown opcode: ioctl still succeeds, 1 consumed");
	check(e.completed == 1 && cq[0].user_data == 0x2000 && cq[0].res == -EINVAL,
	      "  yields a CQE with res == -EINVAL");

	nop(&sq[0], 0x2001);
	sq[0].flags = 1; /* no flags defined */
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "unknown SQE flag bit yields res == -EINVAL");

	nop(&sq[0], 0x2002);
	sq[0].rsvd0 = 7;
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "non-zero SQE rsvd0 yields res == -EINVAL");

	nop(&sq[0], 0x2003);
	sq[0].len = 1; /* NOP reads no argument fields */
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "non-zero unused field yields res == -EINVAL");

	nop(&sq[0], 0x2004);
	sq[0].opcode = XRING_OP_DELAY_NS; /* defined, not yet implemented */
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "unimplemented opcode yields res == -EINVAL");

	/* 3. C1: a mixed batch completes every entry. */
	for (i = 0; i < 8; i++) {
		nop(&sq[i], 0x3000 + i);
		if (i % 2)
			sq[i].opcode = 250;
	}
	enter_init(&e, sq, 8, cq, 16);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 8, "mixed valid/invalid batch: all 8 consumed");
	check(e.completed == 8, "  all 8 completed (C1)");
	ok = 1;
	for (i = 0; i < 8; i++) {
		int64_t want = (i % 2) ? -EINVAL : 0;
		if (cq[i].user_data != 0x3000 + i || cq[i].res != want)
			ok = 0;
	}
	check(ok, "  each completion matches its own SQE");

	/* 4. Short cq_space leaves the remainder queued for the next ENTER. */
	for (i = 0; i < 8; i++)
		nop(&sq[i], 0x4000 + i);
	enter_init(&e, sq, 8, cq, 3);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 8 && e.completed == 3, "cq_space 3 of 8: consumed 8, completed 3");
	check(cq[0].user_data == 0x4000 && cq[2].user_data == 0x4002, "  first three delivered");

	memset(cq, 0, sizeof(cq));
	enter_init(&e, NULL, 0, cq, 16);
	ret = ioctl(fd, XRING_IOC_ENTER, &e);
	check(ret == 0 && e.completed == 5, "drain with to_submit 0 returns the other 5");
	ok = 1;
	for (i = 0; i < 5; i++)
		if (cq[i].user_data != 0x4003 + i)
			ok = 0;
	check(ok, "  and they are the ones left behind, in order");

	/* 5. Protocol failures that do fail the ioctl. */
	enter_init(&e, sq, 1, cq, 16);
	e.flags = 1;
	check_errno(ioctl(fd, XRING_IOC_ENTER, &e), EINVAL, "unknown ENTER flag returns EINVAL");

	enter_init(&e, sq, 1, cq, 16);
	e.reserved[1] = 1;
	check_errno(ioctl(fd, XRING_IOC_ENTER, &e), EINVAL,
		    "non-zero ENTER reserved returns EINVAL");

	enter_init(&e, sq, 1, cq, 16);
	e.rsvd0 = 1;
	check_errno(ioctl(fd, XRING_IOC_ENTER, &e), EINVAL, "non-zero ENTER rsvd0 returns EINVAL");

	enter_init(&e, sq, SQ_ENTRIES + 1, cq, 16);
	check_errno(ioctl(fd, XRING_IOC_ENTER, &e), EINVAL,
		    "to_submit over sq_entries returns EINVAL");

	enter_init(&e, sq, 1, cq, 2);
	e.min_complete = 3;
	check_errno(ioctl(fd, XRING_IOC_ENTER, &e), EINVAL,
		    "min_complete over cq_space returns EINVAL");

	enter_init(&e, sq, 1, NULL, 0);
	e.sq_addr = 0x10; /* unmapped */
	check_errno(ioctl(fd, XRING_IOC_ENTER, &e), EFAULT, "unmapped sq_addr returns EFAULT");
	close(fd);

	/* 6. ENTER before SETUP. */
	fd = open_dev();
	if (fd < 0)
		return 1;
	enter_init(&e, sq, 0, cq, 16);
	check_errno(ioctl(fd, XRING_IOC_ENTER, &e), EINVAL, "ENTER before SETUP returns EINVAL");
	close(fd);

	/* 7. Admission control: the CQ cannot overflow. */
	fd = open_ring();
	if (fd < 0)
		return 1;
	unsigned total = 0;
	for (i = 0; i < CQ_ENTRIES / 8 + 2; i++) {
		unsigned j;
		for (j = 0; j < 8; j++)
			nop(&sq[j], 0x5000 + total + j);
		enter_init(&e, sq, 8, cq, 0); /* submit, never reap */
		ret = ioctl(fd, XRING_IOC_ENTER, &e);
		if (ret < 0) {
			perror("ENTER");
			failures++;
			break;
		}
		total += ret;
		if (ret < 8)
			break; /* short submit: the queue is full */
	}
	check(total == CQ_ENTRIES, "submitting past cq_entries stops at a short count");
	close(fd);

	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
