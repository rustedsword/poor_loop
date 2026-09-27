/* SPDX-License-Identifier: MIT */
#ifndef CHIO_OP_H
#define CHIO_OP_H

#include <liburing.h>

#ifdef __cplusplus
extern "C" {
#endif

struct chio_loop;
struct chio_op;

typedef void chio_complete_fn(struct chio_loop *loop, struct chio_op *op,
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
struct chio_op {
	chio_complete_fn *complete;
	bool pending;
	char data[7];
};

#define CHIO_OP_INIT(fn) { .complete = (fn), .pending = false, .data = {} }

static inline void chio_op_init(struct chio_op *op, chio_complete_fn *complete)
{
	struct chio_op init = CHIO_OP_INIT(complete);

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
[[nodiscard]] struct io_uring_sqe *chio_get_sqe(struct chio_loop *loop, struct chio_op *op);

/* Like chio_get_sqe(), but submit and wait if the SQ is full. */
[[nodiscard]] struct io_uring_sqe *chio_get_sqe_or_submit(struct chio_loop *loop, struct chio_op *op);

/* Check that the SQ has space for 'n' SQEs, otherwise submit and wait. */
[[nodiscard]] int chio_check_sq_space_or_submit(struct chio_loop *loop, unsigned n);

#ifdef __cplusplus
}
#endif

#endif
