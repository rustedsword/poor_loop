/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>

#include "internal.h"

int chio_check_sq_space_or_submit(struct chio_loop *loop, unsigned n)
{
	int ret;

	while (io_uring_sq_space_left(&loop->ring) < n) {
		ret = io_uring_submit(&loop->ring);
		if (ret >= 0)
			ret = io_uring_sqring_wait(&loop->ring);
		if (ret < 0)
			return ret;
	}
	return 0;
}

struct io_uring_sqe *chio_get_sqe(struct chio_loop *loop, struct chio_op *op)
{
	struct io_uring_sqe *sqe;
	int ret;

	if (op->pending) {
		errno = EBUSY;
		return nullptr;
	}
	ret = chio_check_sq_space_or_submit(loop, 1);
	if (ret) {
		errno = -ret;
		return nullptr;
	}
	sqe = io_uring_get_sqe(&loop->ring);
	io_uring_sqe_set_data(sqe, op);
	op->pending = true;
	return sqe;
}

void chio_ops_dispatch(struct chio_loop *loop)
{
	unsigned budget = io_uring_cq_ready(&loop->ring);
	struct io_uring_cqe *cqe;
	struct chio_op *op;

	for (; budget && !io_uring_peek_cqe(&loop->ring, &cqe); budget--) {
		op = io_uring_cqe_get_data(cqe);
		if (op) {
			if (!(cqe->flags & IORING_CQE_F_MORE))
				op->pending = false;
			op->complete(loop, op, cqe);
		}
		io_uring_cqe_seen(&loop->ring, cqe);
	}
}
