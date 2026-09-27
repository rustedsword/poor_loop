/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poll.h>

#include "harness.h"

#ifndef IORING_TIMEOUT_MULTISHOT
#define IORING_TIMEOUT_MULTISHOT (1U << 6)
#endif

#define MANY 1000
#define RESUBMITS 100

static_assert(sizeof(struct chio_op) == sizeof(void *) + 8);
static_assert(offsetof(struct chio_op, pending) == sizeof(void *));
static_assert(offsetof(struct chio_op, data) == sizeof(void *) + 1);
static_assert(sizeof(((struct chio_op *)nullptr)->data) == 7);

static int test_op_init(void)
{
	struct chio_op lit = CHIO_OP_INIT(rec_complete), op;
	static const char zero[7];

	CHECK(lit.complete == rec_complete);
	CHECK(!lit.pending);
	CHECK(!memcmp(lit.data, zero, sizeof(zero)));
	memset(&op, 0xa5, sizeof(op));
	chio_op_init(&op, rec_complete);
	CHECK(op.complete == rec_complete);
	CHECK(!op.pending);
	CHECK(!memcmp(op.data, zero, sizeof(zero)));
	return 0;
}

static int test_nop(void)
{
	static const char pattern[7] = { 1, -2, 3, -4, 5, -6, 7 };
	struct chio_loop loop;
	struct rec rec;

	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	memcpy(rec.op.data, pattern, sizeof(pattern));
	arm_nop(&loop, &rec.op);
	CHECK_EQ(rec.calls, 0);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, 0);
	CHECK(!(rec.flags & IORING_CQE_F_MORE));
	CHECK(rec.op.complete == rec_complete);
	CHECK(!memcmp(rec.op.data, pattern, sizeof(pattern)));
	chio_loop_exit(&loop);
	return 0;
}

static void resubmit_complete(struct chio_loop *loop, struct chio_op *op,
			      const struct io_uring_cqe *cqe)
{
	uint32_t count;

	CHECK_EQ(cqe->res, 0);
	CHECK(!op->pending);
	memcpy(&count, op->data, sizeof(count));
	count++;
	memcpy(op->data, &count, sizeof(count));
	if (count < RESUBMITS)
		arm_nop(loop, op);
	else
		chio_loop_stop(loop);
}

static int test_resubmit(void)
{
	struct chio_op op = CHIO_OP_INIT(resubmit_complete);
	struct chio_loop loop;
	uint32_t count;

	loop_init(&loop, 4);
	arm_nop(&loop, &op);
	CHECK_EQ(chio_loop_run(&loop), 0);
	memcpy(&count, op.data, sizeof(count));
	CHECK_EQ(count, RESUBMITS);
	chio_loop_exit(&loop);
	return 0;
}

static int test_sq_full(void)
{
	struct chio_loop loop;
	struct rec *recs;
	unsigned i;

	recs = calloc(MANY, sizeof(*recs));
	CHECK(recs);
	loop_init(&loop, 4);
	for (i = 0; i < MANY; i++) {
		rec_init(&recs[i], rec_complete);
		arm_nop(&loop, &recs[i].op);
	}
	for (i = 0; i < MANY; i++)
		drain(&loop, &recs[i].op);
	for (i = 0; i < MANY; i++) {
		CHECK_EQ(recs[i].calls, 1);
		CHECK_EQ(recs[i].res, 0);
	}
	chio_loop_exit(&loop);
	free(recs);
	return 0;
}

static int test_submit_error(void)
{
	struct io_uring_params p;
	struct chio_loop loop;
	struct rec recs[3];
	size_t i;

	setup_params(&p);
	p.flags |= IORING_SETUP_R_DISABLED;
	CHECK_EQ(chio_loop_init(&loop, 2, &p), 0);
	for (i = 0; i < ARRAY_SIZE(recs); i++)
		rec_init(&recs[i], rec_complete);
	arm_nop(&loop, &recs[0].op);
	arm_nop(&loop, &recs[1].op);
	errno = 0;
	CHECK(!chio_get_sqe(&loop, &recs[2].op));
	CHECK_EQ(errno, EBADFD);
	CHECK(!recs[2].op.pending);
	enable_ring(&loop);
	drain(&loop, &recs[0].op, &recs[1].op);
	CHECK_EQ(recs[0].calls, 1);
	CHECK_EQ(recs[1].calls, 1);
	CHECK_EQ(recs[2].calls, 0);
	chio_loop_exit(&loop);
	return 0;
}

static int test_double_arm(void)
{
	struct chio_loop loop;
	struct rec rec;

	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	errno = 0;
	CHECK(!chio_get_sqe(&loop, &rec.op));
	CHECK_EQ(errno, EBUSY);
	CHECK(rec.op.pending);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	arm_nop(&loop, &rec.op);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 2);
	chio_loop_exit(&loop);
	return 0;
}

static int test_untracked(void)
{
	struct io_uring_sqe *sqe;
	struct chio_loop loop;
	struct rec rec;

	loop_init(&loop, 8);
	sqe = io_uring_get_sqe(&loop.ring);
	CHECK(sqe);
	io_uring_prep_nop(sqe);
	io_uring_sqe_set_data(sqe, nullptr);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(io_uring_cq_ready(&loop.ring), 0);
	chio_loop_exit(&loop);
	return 0;
}

static int test_op_error(void)
{
	struct chio_loop loop;
	struct rec rec;
	char buf[8];

	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_read(&loop, &rec.op, -1, sizeof(buf), &buf);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, -EBADF);
	chio_loop_exit(&loop);
	return 0;
}

static int test_pipe_io(void)
{
	static const char msg[] = "hello, io_uring";
	struct rec reader, writer;
	struct chio_loop loop;
	char buf[64] = {};
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&writer, rec_complete);
	arm_read(&loop, &reader.op, fds[0], sizeof(buf), &buf);
	io_uring_prep_write(get_sqe(&loop, &writer.op), fds[1], msg,
			    sizeof(msg), 0);
	drain(&loop, &reader.op, &writer.op);
	CHECK_EQ(writer.calls, 1);
	CHECK_EQ(writer.res, sizeof(msg));
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, sizeof(msg));
	CHECK(!memcmp(buf, msg, sizeof(msg)));
	chio_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_linked_timeout(void)
{
	struct __kernel_timespec ts = { .tv_nsec = 1000000 };
	struct io_uring_sqe *sqe;
	struct rec reader, timer;
	struct chio_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&timer, rec_complete);
	sqe = get_sqe(&loop, &reader.op);
	io_uring_prep_read(sqe, fds[0], buf, sizeof(buf), 0);
	sqe->flags |= IOSQE_IO_LINK;
	io_uring_prep_link_timeout(get_sqe(&loop, &timer.op), &ts, 0);
	drain(&loop, &reader.op, &timer.op);
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, -ECANCELED);
	CHECK_EQ(timer.calls, 1);
	CHECK_EQ(timer.res, -ETIME);
	chio_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static void shot_complete(struct chio_loop *loop, struct chio_op *op,
			  const struct io_uring_cqe *cqe)
{
	rec_complete(loop, op, cqe);
	if (cqe->flags & IORING_CQE_F_MORE)
		CHECK_EQ(cqe->res, -ETIME);
}

static int test_multishot(void)
{
	struct __kernel_timespec ts = { .tv_nsec = 1000000 };
	struct chio_loop loop;
	struct rec rec;
	int ret = 0;

	loop_init(&loop, 8);
	rec_init(&rec, shot_complete);
	io_uring_prep_timeout(get_sqe(&loop, &rec.op), &ts, 3,
			      IORING_TIMEOUT_MULTISHOT);
	drain(&loop, &rec.op);
	if (rec.calls == 1 && rec.res == -EINVAL) {
		ret = SKIP;
	} else {
		CHECK_EQ(rec.calls, 3);
		CHECK_EQ(rec.more, 2);
		CHECK_EQ(rec.res, -ETIME);
		CHECK(!(rec.flags & IORING_CQE_F_MORE));
	}
	chio_loop_exit(&loop);
	return ret;
}

struct poll_cancel {
	struct rec poll;
	struct rec cancel;
};

static void poll_complete(struct chio_loop *loop, struct chio_op *op,
			  const struct io_uring_cqe *cqe)
{
	struct poll_cancel *pc = chio_container_of(op, struct poll_cancel,
						   poll.op);

	rec_complete(loop, op, cqe);
	if (!(cqe->flags & IORING_CQE_F_MORE) || pc->poll.more != 1)
		return;
	CHECK(cqe->res & POLLIN);
	io_uring_prep_cancel(get_sqe(loop, &pc->cancel.op), op, 0);
}

static int test_multishot_cancel(void)
{
	struct poll_cancel pc;
	struct chio_loop loop;
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&pc.poll, poll_complete);
	rec_init(&pc.cancel, rec_complete);
	io_uring_prep_poll_multishot(get_sqe(&loop, &pc.poll.op), fds[0],
				     POLLIN);
	CHECK_EQ(io_uring_submit(&loop.ring), 1);
	CHECK_EQ(write(fds[1], "x", 1), 1);
	drain(&loop, &pc.poll.op, &pc.cancel.op);
	CHECK(pc.poll.more >= 1);
	CHECK_EQ(pc.poll.calls, pc.poll.more + 1);
	CHECK_EQ(pc.poll.res, -ECANCELED);
	CHECK(!(pc.poll.flags & IORING_CQE_F_MORE));
	CHECK_EQ(pc.cancel.calls, 1);
	CHECK_EQ(pc.cancel.res, 0);
	chio_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static void cancel_read(bool submitted)
{
	struct rec reader, cancel;
	struct chio_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&cancel, rec_complete);
	arm_read(&loop, &reader.op, fds[0], sizeof(buf), &buf);
	if (submitted)
		CHECK_EQ(io_uring_submit(&loop.ring), 1);
	io_uring_prep_cancel(get_sqe(&loop, &cancel.op), &reader.op, 0);
	drain(&loop, &reader.op, &cancel.op);
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, -ECANCELED);
	CHECK_EQ(cancel.calls, 1);
	CHECK_EQ(cancel.res, 0);
	chio_loop_exit(&loop);
	close_pipe(&fds);
}

static int test_cancel(void)
{
	cancel_read(false);
	cancel_read(true);
	return 0;
}

static int test_cancel_done(void)
{
	struct rec nop, cancel;
	struct chio_loop loop;

	loop_init(&loop, 8);
	rec_init(&nop, rec_complete);
	rec_init(&cancel, rec_complete);
	arm_nop(&loop, &nop.op);
	drain(&loop, &nop.op);
	io_uring_prep_cancel(get_sqe(&loop, &cancel.op), &nop.op, 0);
	drain(&loop, &cancel.op);
	CHECK_EQ(nop.calls, 1);
	CHECK_EQ(nop.res, 0);
	CHECK_EQ(cancel.calls, 1);
	CHECK_EQ(cancel.res, -ENOENT);
	chio_loop_exit(&loop);
	return 0;
}

static int test_cancel_fd(void)
{
	struct rec readers[2], cancel;
	struct chio_loop loop;
	char buf[2][8];
	int fds[2], ret = 0;
	size_t i;

	make_pipe(&fds);
	loop_init(&loop, 8);
	for (i = 0; i < ARRAY_SIZE(readers); i++) {
		rec_init(&readers[i], rec_complete);
		arm_read(&loop, &readers[i].op, fds[0], sizeof(buf[i]),
			 &buf[i]);
	}
	rec_init(&cancel, rec_complete);
	io_uring_prep_cancel_fd(get_sqe(&loop, &cancel.op), fds[0],
				IORING_ASYNC_CANCEL_ALL);
	drain(&loop, &cancel.op);
	if (cancel.res == -EINVAL) {
		ret = SKIP;
	} else {
		CHECK_EQ(cancel.res, ARRAY_SIZE(readers));
		drain(&loop, &readers[0].op, &readers[1].op);
		for (i = 0; i < ARRAY_SIZE(readers); i++) {
			CHECK_EQ(readers[i].calls, 1);
			CHECK_EQ(readers[i].res, -ECANCELED);
		}
	}
	chio_loop_exit(&loop);
	close_pipe(&fds);
	return ret;
}

static int test_cancel_any(void)
{
	struct __kernel_timespec ts = { .tv_sec = 10 };
	struct rec ops[3], cancel;
	struct chio_loop loop;
	int fds[2], ret = 0;
	char buf[8];
	size_t i;

	make_pipe(&fds);
	loop_init(&loop, 8);
	for (i = 0; i < ARRAY_SIZE(ops); i++)
		rec_init(&ops[i], rec_complete);
	rec_init(&cancel, rec_complete);
	arm_read(&loop, &ops[0].op, fds[0], sizeof(buf), &buf);
	io_uring_prep_poll_add(get_sqe(&loop, &ops[1].op), fds[0], POLLIN);
	io_uring_prep_timeout(get_sqe(&loop, &ops[2].op), &ts, 0, 0);
	CHECK_EQ(io_uring_submit(&loop.ring), 3);
	io_uring_prep_cancel(get_sqe(&loop, &cancel.op), nullptr,
			     IORING_ASYNC_CANCEL_ANY);
	drain(&loop, &cancel.op);
	if (cancel.res == -EINVAL) {
		ret = SKIP;
	} else {
		CHECK_EQ(cancel.res, ARRAY_SIZE(ops));
		drain(&loop, &ops[0].op, &ops[1].op, &ops[2].op);
		for (i = 0; i < ARRAY_SIZE(ops); i++) {
			CHECK_EQ(ops[i].calls, 1);
			CHECK_EQ(ops[i].res, -ECANCELED);
		}
	}
	chio_loop_exit(&loop);
	close_pipe(&fds);
	return ret;
}

struct heap_op {
	struct chio_op op;
	int *freed;
};

static void heap_complete(struct chio_loop *loop, struct chio_op *op,
			  const struct io_uring_cqe *cqe)
{
	struct heap_op *heap = chio_container_of(op, struct heap_op, op);
	int *freed = heap->freed;

	CHECK_EQ(cqe->res, 0);
	memset(heap, 0xa5, sizeof(*heap));
	free(heap);
	if (++*freed == 8)
		chio_loop_stop(loop);
}

static int test_free_in_callback(void)
{
	struct heap_op *heap;
	struct chio_loop loop;
	int freed = 0, i;

	loop_init(&loop, 8);
	for (i = 0; i < 8; i++) {
		heap = calloc(1, sizeof(*heap));
		CHECK(heap);
		chio_op_init(&heap->op, heap_complete);
		heap->freed = &freed;
		arm_nop(&loop, &heap->op);
	}
	CHECK_EQ(chio_loop_run(&loop), 0);
	CHECK_EQ(freed, 8);
	chio_loop_exit(&loop);
	return 0;
}

const struct test tests[] = {
	TEST(op_init),
	TEST(nop),
	TEST(resubmit),
	TEST(sq_full),
	TEST(submit_error),
	TEST(double_arm),
	TEST(untracked),
	TEST(op_error),
	TEST(pipe_io),
	TEST(linked_timeout),
	TEST(multishot),
	TEST(multishot_cancel),
	TEST(cancel),
	TEST(cancel_done),
	TEST(cancel_fd),
	TEST(cancel_any),
	TEST(free_in_callback),
};

const size_t tests_count = ARRAY_SIZE(tests);
