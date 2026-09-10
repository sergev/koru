// SPDX-License-Identifier: GPL-2.0
//
// T4 done test: ENTER, NOP only.

#include "koru_test.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Reserved in the ABI but not implemented yet; repoint as opcodes land. */
#define KORU_OP_UNIMPLEMENTED KORU_OP_OPEN /* T9 */

#define SQ_ENTRIES 64
#define CQ_ENTRIES 128

int main(void)
{
	struct koru_sqe sq[16];
	struct koru_cqe cq[16];
	struct koru_enter e;
	int fd, ret;
	unsigned i;

	test_begin(120);

	/* 1. Eight NOPs in, eight CQEs out, user_data matching in order. */
	fd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
	if (fd < 0)
		return 1;
	for (i = 0; i < 8; i++)
		sqe_nop(&sq[i], 0x1000 + i);
	enter_init(&e, sq, 8, cq, 16);
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 8, "ENTER returns 8 SQEs consumed");
	check(e.completed == 8, "  completed == 8");
	check(e.submitted == 8, "  submitted == 8");
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
	sqe_nop(&sq[0], 0x2000);
	sq[0].opcode = 200; /* unknown */
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 1, "unknown opcode: ioctl still succeeds, 1 consumed");
	check(e.completed == 1 && cq[0].user_data == 0x2000 && cq[0].res == -EINVAL,
	      "  yields a CQE with res == -EINVAL");

	sqe_nop(&sq[0], 0x2001);
	sq[0].flags = 1; /* no flags defined */
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "unknown SQE flag bit yields res == -EINVAL");

	sqe_nop(&sq[0], 0x2002);
	sq[0].rsvd0 = 7;
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "non-zero SQE rsvd0 yields res == -EINVAL");

	sqe_nop(&sq[0], 0x2003);
	sq[0].len = 1; /* NOP reads no argument fields */
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "non-zero unused field yields res == -EINVAL");

	sqe_nop(&sq[0], 0x2004);
	sq[0].opcode = KORU_OP_UNIMPLEMENTED;
	enter_init(&e, sq, 1, cq, 16);
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 1 && e.completed == 1 && cq[0].res == -EINVAL,
	      "unimplemented opcode yields res == -EINVAL");

	/* 3. C1: a mixed batch completes every entry. */
	for (i = 0; i < 8; i++) {
		sqe_nop(&sq[i], 0x3000 + i);
		if (i % 2)
			sq[i].opcode = 250;
	}
	enter_init(&e, sq, 8, cq, 16);
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
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
		sqe_nop(&sq[i], 0x4000 + i);
	enter_init(&e, sq, 8, cq, 3);
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 8 && e.completed == 3, "cq_space 3 of 8: consumed 8, completed 3");
	check(cq[0].user_data == 0x4000 && cq[2].user_data == 0x4002, "  first three delivered");

	memset(cq, 0, sizeof(cq));
	enter_init(&e, NULL, 0, cq, 16);
	ret = ioctl(fd, KORU_IOC_ENTER, &e);
	check(ret == 0 && e.completed == 5, "drain with to_submit 0 returns the other 5");
	ok = 1;
	for (i = 0; i < 5; i++)
		if (cq[i].user_data != 0x4003 + i)
			ok = 0;
	check(ok, "  and they are the ones left behind, in order");

	/* 5. Protocol failures that do fail the ioctl. */
	enter_init(&e, sq, 1, cq, 16);
	e.flags = 1;
	check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EINVAL, "unknown ENTER flag returns EINVAL");

	enter_init(&e, sq, 1, cq, 16);
	e.reserved[1] = 1;
	check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EINVAL,
		    "non-zero ENTER reserved returns EINVAL");

	enter_init(&e, sq, SQ_ENTRIES + 1, cq, 16);
	check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EINVAL,
		    "to_submit over sq_entries returns EINVAL");

	enter_init(&e, sq, 1, cq, 2);
	e.min_complete = 3;
	check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EINVAL,
		    "min_complete over cq_space returns EINVAL");

	enter_init(&e, sq, 1, NULL, 0);
	e.sq_addr = 0x10; /* unmapped */
	check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EFAULT, "unmapped sq_addr returns EFAULT");
	close(fd);

	/* 6. ENTER before SETUP. */
	fd = open_dev();
	if (fd < 0)
		return 1;
	enter_init(&e, sq, 0, cq, 16);
	check_errno(ioctl(fd, KORU_IOC_ENTER, &e), EINVAL, "ENTER before SETUP returns EINVAL");
	close(fd);

	/* 7. Admission control: the CQ cannot overflow. */
	fd = open_ring(SQ_ENTRIES, CQ_ENTRIES, 4096, 32);
	if (fd < 0)
		return 1;
	unsigned total = 0;
	for (i = 0; i < CQ_ENTRIES / 8 + 2; i++) {
		unsigned j;
		for (j = 0; j < 8; j++)
			sqe_nop(&sq[j], 0x5000 + total + j);
		enter_init(&e, sq, 8, cq, 0); /* submit, never reap */
		ret = ioctl(fd, KORU_IOC_ENTER, &e);
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

	return test_end();
}
