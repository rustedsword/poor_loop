/* SPDX-License-Identifier: MIT */
#ifndef CHIOLOOP_H
#define CHIOLOOP_H

#include <liburing.h>

#ifdef __cplusplus
extern "C" {
#endif

struct chio_loop;
struct chio_op;

typedef void chio_complete_fn(struct chio_loop *loop, struct chio_op *op,
			      const struct io_uring_cqe *cqe);

struct chio_op {
	chio_complete_fn *complete;
	bool pending;
	char data[7];
};

#define CHIO_OP_INIT(fn) { .complete = (fn), .pending = false, .data = {} }

struct chio_loop {
	struct io_uring ring;
	bool stop;
};

static inline void chio_op_init(struct chio_op *op, chio_complete_fn *complete)
{
	struct chio_op init = CHIO_OP_INIT(complete);

	*op = init;
}

int chio_loop_init(struct chio_loop *loop, unsigned entries,
		   struct io_uring_params *params);
void chio_loop_exit(struct chio_loop *loop);
struct io_uring_sqe *chio_get_sqe(struct chio_loop *loop, struct chio_op *op);
int chio_loop_run_once(struct chio_loop *loop, bool wait);
int chio_loop_run(struct chio_loop *loop);
void chio_loop_stop(struct chio_loop *loop);

#ifdef __cplusplus
}
#endif

#endif
