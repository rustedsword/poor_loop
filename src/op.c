/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>

#include "internal.h"

int poor_loop_check_sq_space_or_submit(struct poor_loop *loop, unsigned n)
{
	unsigned space = io_uring_sq_space_left(&loop->ring);
	int ret;

	if (uring_likely(space >= n))
		return 0;
	poor_loop_log(loop, "SQ has %u of %u entries free, %u needed: submitting", space, loop->ring.sq.ring_entries, n);
	do {
		ret = io_uring_submit(&loop->ring);
		if (ret >= 0)
			ret = io_uring_sqring_wait(&loop->ring);
		if (ret < 0)
			return ret;
	} while (io_uring_sq_space_left(&loop->ring) < n);
	return 0;
}

struct io_uring_sqe *poor_loop_get_sqe(struct poor_loop *loop, struct poor_loop_op *op)
{
	struct io_uring_sqe *sqe;

	if (uring_unlikely(op->pending)) {
		errno = EBUSY;
		return nullptr;
	}
	sqe = io_uring_get_sqe(&loop->ring);
	if (uring_unlikely(!sqe)) {
		errno = EAGAIN;
		return nullptr;
	}
	io_uring_sqe_set_data(sqe, op);
	op->pending = true;
	return sqe;
}

struct io_uring_sqe *poor_loop_get_untracked_sqe(struct poor_loop *loop)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&loop->ring);

	if (uring_unlikely(!sqe)) {
		errno = EAGAIN;
		return nullptr;
	}
	io_uring_sqe_set_data(sqe, nullptr);
	return sqe;
}

struct io_uring_sqe *poor_loop_get_untracked_sqe_or_submit(struct poor_loop *loop)
{
	struct io_uring_sqe *sqe = poor_loop_get_untracked_sqe(loop);
	int ret;

	if (uring_likely(sqe))
		return sqe;
	ret = poor_loop_check_sq_space_or_submit(loop, 1);
	if (ret) {
		errno = -ret;
		return nullptr;
	}
	return poor_loop_get_untracked_sqe(loop);
}

struct io_uring_sqe *poor_loop_get_sqe_or_submit(struct poor_loop *loop, struct poor_loop_op *op)
{
	struct io_uring_sqe *sqe = poor_loop_get_sqe(loop, op);
	int ret;

	if (uring_likely(sqe) || errno != EAGAIN)
		return sqe;
	ret = poor_loop_check_sq_space_or_submit(loop, 1);
	if (ret) {
		errno = -ret;
		return nullptr;
	}
	return poor_loop_get_sqe(loop, op);
}

void poor_loop_ops_dispatch(struct poor_loop *loop)
{
	unsigned budget = io_uring_cq_ready(&loop->ring);
	struct io_uring_cqe *cqe;
	struct poor_loop_op *op;

	for (; budget && !io_uring_peek_cqe(&loop->ring, &cqe); budget--) {
		op = io_uring_cqe_get_data(cqe);
		if (uring_likely(op)) {
			if (!(cqe->flags & IORING_CQE_F_MORE))
				op->pending = false;
			op->complete(loop, op, cqe);
		}
		io_uring_cqe_seen(&loop->ring, cqe);
	}
}
