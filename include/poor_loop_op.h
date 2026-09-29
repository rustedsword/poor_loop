/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_OP_H
#define POOR_LOOP_OP_H

#include <liburing.h>

#ifdef __cplusplus
extern "C" {
#endif

struct poor_loop;
struct poor_loop_op;

typedef void poor_loop_complete_fn(struct poor_loop *loop,
				   struct poor_loop_op *op,
				   const struct io_uring_cqe *cqe);

/*
 * Tracks an in-flight I/O request.
 *
 * Keep the op alive as long as 'pending' is true. Some requests can call
 * complete() multiple times; once 'pending' is false, the operation is fully
 * done and you can safely reuse the op for a new request or free it.
 *
 * 'data' is scratch space for caller use.
 */
struct poor_loop_op {
	poor_loop_complete_fn *complete;
	bool pending;
	char data[7];
};

#define POOR_LOOP_OP_INIT(fn) { .complete = (fn), .pending = false, .data = {} }

static inline void poor_loop_op_init(struct poor_loop_op *op,
				     poor_loop_complete_fn *complete)
{
	struct poor_loop_op init = POOR_LOOP_OP_INIT(complete);

	*op = init;
}

/*
 * Acquire an SQE bound to 'op' and mark the op pending.
 *
 * Prepare the SQE with any liburing helper, but do not overwrite
 * sqe->user_data and do not set IOSQE_CQE_SKIP_SUCCESS (otherwise the op will
 * never complete).
 *
 * Returns the SQE on success, or nullptr on failure with errno set: EBUSY if
 * 'op' is already pending, EAGAIN if the SQ is full.
 */
[[nodiscard]] struct io_uring_sqe *poor_loop_get_sqe(struct poor_loop *loop, struct poor_loop_op *op);

/* Like poor_loop_get_sqe(), but submit and wait if the SQ is full. */
[[nodiscard]] struct io_uring_sqe *poor_loop_get_sqe_or_submit(struct poor_loop *loop, struct poor_loop_op *op);

/*
 * Like poor_loop_get_sqe(), but with zero user_data, so the loop ignores
 * the CQE.
 */
[[nodiscard]] struct io_uring_sqe *poor_loop_get_untracked_sqe(struct poor_loop *loop);

/* Like poor_loop_get_untracked_sqe(), but submit and wait if the SQ is full. */
[[nodiscard]] struct io_uring_sqe *poor_loop_get_untracked_sqe_or_submit(struct poor_loop *loop);

/* Check that the SQ has space for 'n' SQEs, otherwise submit and wait. */
[[nodiscard]] int poor_loop_check_sq_space_or_submit(struct poor_loop *loop, unsigned n);

#ifdef __cplusplus
}
#endif

#endif
