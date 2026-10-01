/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_H
#define POOR_LOOP_H

#include <liburing.h>

#include "poor_loop_log.h"
#include "poor_loop_op.h"
#include "poor_loop_timer.h"
#include "poor_loop_uring_array.h"

#ifdef __cplusplus
extern "C" {
#endif

struct poor_loop {
	struct io_uring ring;
	poor_loop_timer_list timers;
	bool stop;
};

/*
 * Return the ring for direct use with liburing.
 *
 * The loop owns the CQ, so never consume CQEs.
 */
static inline struct io_uring *poor_loop_ring(struct poor_loop *loop)
{
	return &loop->ring;
}

/*
 * Initialize the event loop.
 *
 * 'params' is passed to io_uring_queue_init_params(), which writes the actual
 * ring sizes back into it. Returns 0 on success, or -errno on error.
 */
[[nodiscard]] int poor_loop_init(struct poor_loop *loop, unsigned entries, struct io_uring_params *params);

/*
 * Close the io_uring ring without submitting cancellations or running callbacks.
 */
static inline void poor_loop_exit(struct poor_loop *loop)
{
	io_uring_queue_exit(&loop->ring);
}

/*
 * Run one iteration of the event loop, regardless of the stop flag.
 *
 * Callers driving their own loop can check poor_loop_stopped() between
 * iterations or use their own termination condition. This function does not
 * clear the flag.
 *
 * Callbacks must not call poor_loop_run(), poor_loop_run_once() or
 * poor_loop_exit(). CQEs with zero user_data are ignored.
 *
 * Returns 0, or -errno if waiting failed.
 */
[[nodiscard]] int poor_loop_run_once(struct poor_loop *loop);

/* Make poor_loop_run() return after the current iteration. */
static inline void poor_loop_stop(struct poor_loop *loop)
{
	loop->stop = true;
}

/* Return whether a stop has been requested. */
static inline bool poor_loop_stopped(const struct poor_loop *loop)
{
	return loop->stop;
}

/* Clear the stop flag so poor_loop_run() can run again. */
static inline void poor_loop_clear_stop(struct poor_loop *loop)
{
	loop->stop = false;
}

/*
 * Run the event loop until poor_loop_stop() is called.
 *
 * The stop flag stays set on return; call poor_loop_clear_stop() to run again.
 *
 * Returns 0 on normal exit, or -errno if waiting failed.
 */
[[nodiscard]] static inline int poor_loop_run(struct poor_loop *loop)
{
	int ret = 0;

	while (!poor_loop_stopped(loop) && !ret)
		ret = poor_loop_run_once(loop);
	return ret;
}

#ifdef __cplusplus
}
#endif

#endif
