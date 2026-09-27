/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>

#include "chioloop.h"

int chio_loop_init(struct chio_loop *loop, unsigned entries,
		   struct io_uring_params *params)
{
	loop->stop = false;
	return io_uring_queue_init_params(entries, &loop->ring, params);
}

void chio_loop_exit(struct chio_loop *loop)
{
	io_uring_queue_exit(&loop->ring);
}

struct io_uring_sqe *chio_get_sqe(struct chio_loop *loop, struct chio_op *op)
{
	struct io_uring_sqe *sqe;
	int ret;

	if (op->pending) {
		errno = EBUSY;
		return nullptr;
	}
	while (!(sqe = io_uring_get_sqe(&loop->ring))) {
		ret = io_uring_submit(&loop->ring);
		if (ret >= 0)
			ret = io_uring_sqring_wait(&loop->ring);
		if (ret < 0) {
			errno = -ret;
			return nullptr;
		}
	}
	io_uring_sqe_set_data(sqe, op);
	op->pending = true;
	return sqe;
}

static int dispatch(struct chio_loop *loop)
{
	unsigned budget = io_uring_cq_ready(&loop->ring);
	struct io_uring_cqe *cqe;
	struct chio_op *op;
	int count = 0;

	for (; budget && !io_uring_peek_cqe(&loop->ring, &cqe); budget--) {
		op = io_uring_cqe_get_data(cqe);
		if (op) {
			if (!(cqe->flags & IORING_CQE_F_MORE))
				op->pending = false;
			op->complete(loop, op, cqe);
		}
		io_uring_cqe_seen(&loop->ring, cqe);
		count++;
	}
	return count;
}

int chio_loop_run_once(struct chio_loop *loop, bool wait)
{
	int ret, count;

	if (wait)
		ret = io_uring_submit_and_wait(&loop->ring, 1);
	else
		ret = io_uring_submit_and_get_events(&loop->ring);
	count = dispatch(loop);
	if (ret < 0 && ret != -EINTR && ret != -EBUSY)
		return ret;
	return count;
}

int chio_loop_run(struct chio_loop *loop)
{
	int ret = 0;

	while (!loop->stop && ret >= 0)
		ret = chio_loop_run_once(loop, true);
	loop->stop = false;
	return ret < 0 ? ret : 0;
}

void chio_loop_stop(struct chio_loop *loop)
{
	loop->stop = true;
}
