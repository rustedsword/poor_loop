/* SPDX-License-Identifier: MIT */
#ifndef CHIOLOOP_H
#define CHIOLOOP_H

#include <liburing.h>
#include <stdint.h>
#include <time.h>

#include "chio_list.h"

#ifdef __cplusplus
extern "C" {
#endif

struct chio_loop;
struct chio_op;
struct chio_timer;

typedef void chio_complete_fn(struct chio_loop *loop, struct chio_op *op,
			      const struct io_uring_cqe *cqe);
typedef void chio_timer_fn(struct chio_loop *loop, struct chio_timer *timer);

struct chio_op {
	chio_complete_fn *complete;
	bool pending;
	char data[7];
};

#define CHIO_OP_INIT(fn) { .complete = (fn), .pending = false, .data = {} }

struct chio_timer {
	struct chio_list link;
	uint64_t deadline;
	chio_timer_fn *fire;
};

#define CHIO_TIMER_INIT(fn) { .link = {}, .deadline = 0, .fire = (fn) }

struct chio_loop {
	struct io_uring ring;
	struct chio_list timers;
	bool stop;
};

static inline void chio_op_init(struct chio_op *op, chio_complete_fn *complete)
{
	struct chio_op init = CHIO_OP_INIT(complete);

	*op = init;
}

static inline void chio_timer_init(struct chio_timer *timer,
				   chio_timer_fn *fire)
{
	struct chio_timer init = CHIO_TIMER_INIT(fire);

	*timer = init;
}

static inline bool chio_timer_armed(const struct chio_timer *timer)
{
	return chio_list_linked(&timer->link);
}

static inline void chio_timer_disarm(struct chio_timer *timer)
{
	if (chio_timer_armed(timer))
		chio_list_remove(&timer->link);
}

static inline uint64_t chio_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1'000'000'000 + (uint64_t)ts.tv_nsec;
}

int chio_loop_init(struct chio_loop *loop, unsigned entries,
		   struct io_uring_params *params);
void chio_loop_exit(struct chio_loop *loop);
struct io_uring_sqe *chio_get_sqe(struct chio_loop *loop, struct chio_op *op);
void chio_timer_arm(struct chio_loop *loop, struct chio_timer *timer,
		    uint64_t deadline);
int chio_loop_run(struct chio_loop *loop);
void chio_loop_stop(struct chio_loop *loop);

#ifdef __cplusplus
}
#endif

#endif
