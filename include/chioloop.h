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

/*
 * Software timer.
 *
 * 'deadline' is an absolute CLOCK_MONOTONIC timestamp in nanoseconds (see
 * chio_now()). The timer is automatically disarmed before fire() runs, so the
 * callback is free to re-arm it.
 *
 * An armed timer must be disarmed before it is freed.
 */
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

/*
 * Initialize the event loop.
 *
 * 'params' is passed to io_uring_queue_init_params(), which writes the actual
 * ring sizes back into it. Returns 0 on success, or -errno on error.
 */
[[nodiscard]] int chio_loop_init(struct chio_loop *loop, unsigned entries,
				 struct io_uring_params *params);

/*
 * Tear down the event loop and close the io_uring ring.
 *
 * All timers must be disarmed before calling this. complete() is never called
 * for pending ops; the kernel cancels their requests asynchronously after the
 * ring is closed.
 */
void chio_loop_exit(struct chio_loop *loop);

/*
 * Acquire an SQE bound to 'op' and mark the op pending.
 *
 * Prepare the SQE with any liburing helper, but do not overwrite
 * sqe->user_data and do not set IOSQE_CQE_SKIP_SUCCESS (otherwise the op will
 * never complete). If the SQ is full, queued SQEs are submitted first to make
 * room.
 *
 * Returns the SQE on success, or nullptr on failure with errno set (e.g. EBUSY
 * if 'op' is already pending).
 */
[[nodiscard]] struct io_uring_sqe *chio_get_sqe(struct chio_loop *loop,
						struct chio_op *op);

/*
 * Arm or re-arm a timer. Timers with equal deadlines fire in the order they
 * were armed.
 */
void chio_timer_arm(struct chio_loop *loop, struct chio_timer *timer,
		    uint64_t deadline);

/*
 * Run the event loop until chio_loop_stop() is called.
 *
 * Callbacks must not call chio_loop_run() or chio_loop_exit(). CQEs with zero
 * user_data are ignored.
 *
 * Returns 0 on normal exit, or -errno if waiting failed.
 */
[[nodiscard]] int chio_loop_run(struct chio_loop *loop);

/*
 * Stop the event loop.
 *
 * If called from a callback, chio_loop_run() will exit after the current
 * iteration finishes.
 */
void chio_loop_stop(struct chio_loop *loop);

#ifdef __cplusplus
}
#endif

#endif
