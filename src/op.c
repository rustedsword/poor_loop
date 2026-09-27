/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>

#include "internal.h"

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
