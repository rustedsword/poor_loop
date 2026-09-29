/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poll.h>

#include "harness.h"

#ifndef IORING_TIMEOUT_MULTISHOT
#define IORING_TIMEOUT_MULTISHOT (1U << 6)
#endif

#define MANY 1000
#define RESUBMITS 100

static_assert(sizeof(struct poor_loop_op) == sizeof(void *) + 8);
static_assert(offsetof(struct poor_loop_op, pending) == sizeof(void *));
static_assert(offsetof(struct poor_loop_op, data) == sizeof(void *) + 1);
static_assert(sizeof(((struct poor_loop_op *)nullptr)->data) == 7);

static int test_op_init(void)
{
	struct poor_loop_op lit = POOR_LOOP_OP_INIT(rec_complete), op;

	CHECK(lit.complete == rec_complete);
	CHECK(!lit.pending);
	foreach_array_ref(lit.data, byte)
		CHECK_EQ(*byte, 0);
	memset(&op, 0xa5, sizeof(op));
	poor_loop_op_init(&op, rec_complete);
	CHECK(op.complete == rec_complete);
	CHECK(!op.pending);
	foreach_array_ref(op.data, byte)
		CHECK_EQ(*byte, 0);
	return 0;
}

static int test_nop(void)
{
	static const char pattern[7] = { 1, -2, 3, -4, 5, -6, 7 };
	struct poor_loop loop;
	struct rec rec;

	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	copy_array(rec.op.data, pattern);
	arm_nop(&loop, &rec.op);
	CHECK_EQ(rec.calls, 0);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, 0);
	CHECK(!(rec.flags & IORING_CQE_F_MORE));
	CHECK(rec.op.complete == rec_complete);
	CHECK(!memcmp(rec.op.data, pattern, ARRAY_SIZE_BYTES(pattern)));
	poor_loop_exit(&loop);
	return 0;
}

static void resubmit_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
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
		poor_loop_stop(loop);
}

static int test_resubmit(void)
{
	struct poor_loop_op op = POOR_LOOP_OP_INIT(resubmit_complete);
	struct poor_loop loop;
	uint32_t count;

	loop_init(&loop, 4);
	arm_nop(&loop, &op);
	CHECK_EQ(poor_loop_run(&loop), 0);
	memcpy(&count, op.data, sizeof(count));
	CHECK_EQ(count, RESUBMITS);
	poor_loop_exit(&loop);
	return 0;
}

static int test_sq_full(void)
{
	struct rec(*recs)[MANY] = calloc_array(recs);
	struct poor_loop loop;

	CHECK(recs);
	loop_init(&loop, 4);
	foreach_array_ref(recs, rec) {
		rec_init(rec, rec_complete);
		arm_nop(&loop, &rec->op);
	}
	foreach_array_ref(recs, rec)
		drain(&loop, &rec->op);
	foreach_array_ref(recs, rec) {
		CHECK_EQ(rec->calls, 1);
		CHECK_EQ(rec->res, 0);
	}
	poor_loop_exit(&loop);
	free(recs);
	return 0;
}

static int test_sq_space(void)
{
	struct poor_loop loop;
	struct io_uring *ring;
	struct rec rec;

	loop_init(&loop, 4);
	ring = poor_loop_ring(&loop);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	CHECK_EQ(poor_loop_check_sq_space_or_submit(&loop, 3), 0);
	CHECK_EQ(io_uring_sq_ready(ring), 1);
	CHECK_EQ(poor_loop_check_sq_space_or_submit(&loop, 4), 0);
	CHECK_EQ(io_uring_sq_space_left(ring), 4);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, 0);
	poor_loop_exit(&loop);
	return 0;
}

static int test_get_sqe_full(void)
{
	struct poor_loop loop;
	struct rec recs[5];

	loop_init(&loop, 4);
	foreach_array_ref(recs, rec)
		rec_init(rec, rec_complete);
	foreach_array_ref(arrview_first(4, recs), rec)
		arm_nop(&loop, &rec->op);
	errno = 0;
	CHECK(!poor_loop_get_sqe(&loop, &recs[4].op));
	CHECK_EQ(errno, EAGAIN);
	CHECK(!recs[4].op.pending);
	errno = 0;
	CHECK(!poor_loop_get_sqe_or_submit(&loop, &recs[0].op));
	CHECK_EQ(errno, EBUSY);
	CHECK_EQ(io_uring_sq_ready(poor_loop_ring(&loop)), 4);
	arm_nop(&loop, &recs[4].op);
	foreach_array_ref(recs, rec)
		drain(&loop, &rec->op);
	foreach_array_ref(recs, rec) {
		CHECK_EQ(rec->calls, 1);
		CHECK_EQ(rec->res, 0);
	}
	poor_loop_exit(&loop);
	return 0;
}

static int test_submit_error(void)
{
	struct io_uring_params p;
	struct poor_loop loop;
	struct rec recs[3];

	setup_params(&p);
	p.flags |= IORING_SETUP_R_DISABLED;
	CHECK_EQ(poor_loop_init(&loop, 2, &p), 0);
	foreach_array_ref(recs, rec)
		rec_init(rec, rec_complete);
	arm_nop(&loop, &recs[0].op);
	arm_nop(&loop, &recs[1].op);
	CHECK_EQ(poor_loop_check_sq_space_or_submit(&loop, 1), -EBADFD);
	errno = 0;
	CHECK(!poor_loop_get_sqe_or_submit(&loop, &recs[2].op));
	CHECK_EQ(errno, EBADFD);
	errno = 0;
	CHECK(!poor_loop_get_untracked_sqe_or_submit(&loop));
	CHECK_EQ(errno, EBADFD);
	CHECK(!recs[2].op.pending);
	enable_ring(&loop);
	drain(&loop, &recs[0].op, &recs[1].op);
	CHECK_EQ(recs[0].calls, 1);
	CHECK_EQ(recs[1].calls, 1);
	CHECK_EQ(recs[2].calls, 0);
	poor_loop_exit(&loop);
	return 0;
}

static int test_double_arm(void)
{
	struct poor_loop loop;
	struct rec rec;

	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	errno = 0;
	CHECK(!poor_loop_get_sqe(&loop, &rec.op));
	CHECK_EQ(errno, EBUSY);
	errno = 0;
	CHECK(!poor_loop_get_sqe_or_submit(&loop, &rec.op));
	CHECK_EQ(errno, EBUSY);
	CHECK(rec.op.pending);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	arm_nop(&loop, &rec.op);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 2);
	poor_loop_exit(&loop);
	return 0;
}

static int test_untracked(void)
{
	struct io_uring_sqe *sqe;
	struct poor_loop loop;
	struct rec recs[5];

	loop_init(&loop, 4);
	foreach_array_ref(recs, rec)
		rec_init(rec, rec_complete);
	foreach_array_ref(arrview_first(4, recs), rec)
		arm_nop(&loop, &rec->op);
	errno = 0;
	CHECK(!poor_loop_get_untracked_sqe(&loop));
	CHECK_EQ(errno, EAGAIN);
	foreach_array_ref(arrview_first(4, recs), rec)
		drain(&loop, &rec->op);
	sqe = poor_loop_get_untracked_sqe(&loop);
	CHECK(sqe);
	CHECK_EQ(sqe->user_data, 0);
	io_uring_prep_nop(sqe);
	arm_nop(&loop, &recs[4].op);
	drain(&loop, &recs[4].op);
	foreach_array_ref(recs, rec)
		CHECK_EQ(rec->calls, 1);
	CHECK_EQ(io_uring_cq_ready(poor_loop_ring(&loop)), 0);
	poor_loop_exit(&loop);
	return 0;
}

static int test_untracked_or_submit(void)
{
	struct io_uring_sqe *sqe;
	struct poor_loop loop;
	struct rec recs[5];

	loop_init(&loop, 4);
	foreach_array_ref(recs, rec)
		rec_init(rec, rec_complete);
	foreach_array_ref(arrview_first(4, recs), rec)
		arm_nop(&loop, &rec->op);
	sqe = poor_loop_get_untracked_sqe_or_submit(&loop);
	CHECK(sqe);
	CHECK_EQ(sqe->user_data, 0);
	io_uring_prep_nop(sqe);
	arm_nop(&loop, &recs[4].op);
	foreach_array_ref(recs, rec)
		drain(&loop, &rec->op);
	foreach_array_ref(recs, rec) {
		CHECK_EQ(rec->calls, 1);
		CHECK_EQ(rec->res, 0);
	}
	CHECK_EQ(io_uring_cq_ready(poor_loop_ring(&loop)), 0);
	poor_loop_exit(&loop);
	return 0;
}

static int test_op_error(void)
{
	struct poor_loop loop;
	struct rec rec;
	char buf[8];

	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_read(&loop, &rec.op, -1, buf);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, -EBADF);
	poor_loop_exit(&loop);
	return 0;
}

static int test_pipe_io(void)
{
	static const char msg[] = "hello, io_uring";
	struct rec reader, writer;
	struct poor_loop loop;
	char buf[64] = {};
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&writer, rec_complete);
	arm_read(&loop, &reader.op, fds[0], buf);
	io_uring_prep_write(get_sqe(&loop, &writer.op), fds[1], msg, ARRAY_SIZE_BYTES(msg), 0);
	drain(&loop, &reader.op, &writer.op);
	CHECK_EQ(writer.calls, 1);
	CHECK_EQ(writer.res, ARRAY_SIZE_BYTES(msg));
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, ARRAY_SIZE_BYTES(msg));
	CHECK(!memcmp(buf, msg, ARRAY_SIZE_BYTES(msg)));
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_linked_timeout(void)
{
	struct __kernel_timespec ts = { .tv_nsec = 1000000 };
	struct io_uring_sqe *sqe;
	struct rec reader, timer;
	struct poor_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&timer, rec_complete);
	sqe = get_sqe(&loop, &reader.op);
	io_uring_prep_read(sqe, fds[0], buf, ARRAY_SIZE_BYTES(buf), 0);
	sqe->flags |= IOSQE_IO_LINK;
	io_uring_prep_link_timeout(get_sqe(&loop, &timer.op), &ts, 0);
	drain(&loop, &reader.op, &timer.op);
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, -ECANCELED);
	CHECK_EQ(timer.calls, 1);
	CHECK_EQ(timer.res, -ETIME);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_linked_sq_full(void)
{
	struct io_uring_sqe *sqe;
	struct poor_loop loop;
	struct rec recs[6];
	char buf[8];

	loop_init(&loop, 4);
	foreach_array_ref(recs, rec)
		rec_init(rec, rec_complete);
	foreach_array_ref(arrview_first(3, recs), rec)
		arm_nop(&loop, &rec->op);
	CHECK_EQ(poor_loop_check_sq_space_or_submit(&loop, 3), 0);
	sqe = get_sqe(&loop, &recs[3].op);
	io_uring_prep_read(sqe, -1, buf, ARRAY_SIZE_BYTES(buf), 0);
	sqe->flags |= IOSQE_IO_LINK;
	sqe = get_sqe(&loop, &recs[4].op);
	io_uring_prep_nop(sqe);
	sqe->flags |= IOSQE_IO_LINK;
	arm_nop(&loop, &recs[5].op);
	foreach_array_ref(recs, rec)
		drain(&loop, &rec->op);
	foreach_array_ref(arrview_first(3, recs), rec)
		CHECK_EQ(rec->res, 0);
	CHECK_EQ(recs[3].res, -EBADF);
	CHECK_EQ(recs[4].res, -ECANCELED);
	CHECK_EQ(recs[5].res, -ECANCELED);
	foreach_array_ref(recs, rec)
		CHECK_EQ(rec->calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

static void shot_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	rec_complete(loop, op, cqe);
	if (cqe->flags & IORING_CQE_F_MORE)
		CHECK_EQ(cqe->res, -ETIME);
}

static int test_multishot(void)
{
	struct __kernel_timespec ts = { .tv_nsec = 1000000 };
	struct poor_loop loop;
	struct rec rec;
	int ret = 0;

	loop_init(&loop, 8);
	rec_init(&rec, shot_complete);
	io_uring_prep_timeout(get_sqe(&loop, &rec.op), &ts, 3, IORING_TIMEOUT_MULTISHOT);
	drain(&loop, &rec.op);
	if (rec.calls == 1 && rec.res == -EINVAL) {
		ret = SKIP;
	} else {
		CHECK_EQ(rec.calls, 3);
		CHECK_EQ(rec.more, 2);
		CHECK_EQ(rec.res, -ETIME);
		CHECK(!(rec.flags & IORING_CQE_F_MORE));
	}
	poor_loop_exit(&loop);
	return ret;
}

struct poll_cancel {
	struct rec poll;
	struct rec cancel;
};

static void poll_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct poll_cancel *pc = container_of(op, struct poll_cancel, poll.op);

	rec_complete(loop, op, cqe);
	if (!(cqe->flags & IORING_CQE_F_MORE) || pc->poll.more != 1)
		return;
	CHECK(cqe->res & POLLIN);
	io_uring_prep_cancel(get_sqe(loop, &pc->cancel.op), op, 0);
}

static int test_multishot_cancel(void)
{
	struct poll_cancel pc;
	struct poor_loop loop;
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&pc.poll, poll_complete);
	rec_init(&pc.cancel, rec_complete);
	io_uring_prep_poll_multishot(get_sqe(&loop, &pc.poll.op), fds[0], POLLIN);
	submit(&loop, 1);
	CHECK_EQ(write(fds[1], "x", 1), 1);
	drain(&loop, &pc.poll.op, &pc.cancel.op);
	CHECK(pc.poll.more >= 1);
	CHECK_EQ(pc.poll.calls, pc.poll.more + 1);
	CHECK_EQ(pc.poll.res, -ECANCELED);
	CHECK(!(pc.poll.flags & IORING_CQE_F_MORE));
	CHECK_EQ(pc.cancel.calls, 1);
	CHECK_EQ(pc.cancel.res, 0);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static void cancel_read(bool submitted)
{
	struct rec reader, cancel;
	struct poor_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&cancel, rec_complete);
	arm_read(&loop, &reader.op, fds[0], buf);
	if (submitted)
		submit(&loop, 1);
	io_uring_prep_cancel(get_sqe(&loop, &cancel.op), &reader.op, 0);
	drain(&loop, &reader.op, &cancel.op);
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, -ECANCELED);
	CHECK_EQ(cancel.calls, 1);
	CHECK_EQ(cancel.res, 0);
	poor_loop_exit(&loop);
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
	struct poor_loop loop;

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
	poor_loop_exit(&loop);
	return 0;
}

static int test_cancel_fd(void)
{
	struct rec readers[2], cancel;
	struct poor_loop loop;
	char buf[2][8];
	int fds[2], ret = 0;

	make_pipe(&fds);
	loop_init(&loop, 8);
	foreach_array_index(readers, i) {
		rec_init(&readers[i], rec_complete);
		arm_read(&loop, &readers[i].op, fds[0], buf[i]);
	}
	rec_init(&cancel, rec_complete);
	io_uring_prep_cancel_fd(get_sqe(&loop, &cancel.op), fds[0], IORING_ASYNC_CANCEL_ALL);
	drain(&loop, &cancel.op);
	if (cancel.res == -EINVAL) {
		ret = SKIP;
	} else {
		CHECK_EQ(cancel.res, ARRAY_SIZE(readers));
		drain(&loop, &readers[0].op, &readers[1].op);
		foreach_array_ref(readers, reader) {
			CHECK_EQ(reader->calls, 1);
			CHECK_EQ(reader->res, -ECANCELED);
		}
	}
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return ret;
}

static int test_cancel_any(void)
{
	struct __kernel_timespec ts = { .tv_sec = 10 };
	struct rec ops[3], cancel;
	struct poor_loop loop;
	int fds[2], ret = 0;
	char buf[8];

	make_pipe(&fds);
	loop_init(&loop, 8);
	foreach_array_ref(ops, rec)
		rec_init(rec, rec_complete);
	rec_init(&cancel, rec_complete);
	arm_read(&loop, &ops[0].op, fds[0], buf);
	io_uring_prep_poll_add(get_sqe(&loop, &ops[1].op), fds[0], POLLIN);
	io_uring_prep_timeout(get_sqe(&loop, &ops[2].op), &ts, 0, 0);
	submit(&loop, 3);
	io_uring_prep_cancel(get_sqe(&loop, &cancel.op), nullptr, IORING_ASYNC_CANCEL_ANY);
	drain(&loop, &cancel.op);
	if (cancel.res == -EINVAL) {
		ret = SKIP;
	} else {
		CHECK_EQ(cancel.res, ARRAY_SIZE(ops));
		drain(&loop, &ops[0].op, &ops[1].op, &ops[2].op);
		foreach_array_ref(ops, rec) {
			CHECK_EQ(rec->calls, 1);
			CHECK_EQ(rec->res, -ECANCELED);
		}
	}
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return ret;
}

struct heap_op {
	struct poor_loop_op op;
	int *freed;
};

static void heap_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct heap_op *heap = container_of(op, struct heap_op, op);
	int *freed = heap->freed;

	CHECK_EQ(cqe->res, 0);
	memset(heap, 0xa5, sizeof(*heap));
	free(heap);
	if (++*freed == 8)
		poor_loop_stop(loop);
}

static int test_free_in_callback(void)
{
	struct heap_op *heap;
	struct poor_loop loop;
	int freed = 0, i;

	loop_init(&loop, 8);
	for (i = 0; i < 8; i++) {
		heap = calloc(1, sizeof(*heap));
		CHECK(heap);
		poor_loop_op_init(&heap->op, heap_complete);
		heap->freed = &freed;
		arm_nop(&loop, &heap->op);
	}
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(freed, 8);
	poor_loop_exit(&loop);
	return 0;
}

static const struct test tests[] = {
	TEST(op_init),
	TEST(nop),
	TEST(resubmit),
	TEST(sq_full),
	TEST(sq_space),
	TEST(get_sqe_full),
	TEST(submit_error),
	TEST(double_arm),
	TEST(untracked),
	TEST(untracked_or_submit),
	TEST(op_error),
	TEST(pipe_io),
	TEST(linked_timeout),
	TEST(linked_sq_full),
	TEST(multishot),
	TEST(multishot_cancel),
	TEST(cancel),
	TEST(cancel_done),
	TEST(cancel_fd),
	TEST(cancel_any),
	TEST(free_in_callback),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
