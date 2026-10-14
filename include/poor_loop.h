/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_H
#define POOR_LOOP_H

#include <errno.h>
#include <liburing.h>

#include "poor_loop_log.h"
#include "poor_loop_op.h"
#include "poor_loop_timer.h"
#include "poor_loop_tsan.h"
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

[[nodiscard]] int poor_loop_init_impl(struct poor_loop *loop, unsigned entries, struct io_uring_params *params);

/*
 * Initialize the event loop.
 *
 * 'params' is passed to io_uring_queue_init_params(), which writes the actual
 * ring sizes back into it. IORING_SETUP_SQ_REWIND is not supported yet.
 * Returns 0 on success, or -errno on error.
 */
[[nodiscard]] static inline int poor_loop_init(struct poor_loop *loop, unsigned entries, struct io_uring_params *params)
{
	int ret = poor_loop_init_impl(loop, entries, params);

	if (!ret)
		poor_loop_tsan_ring_acquire(&loop->ring);
	return ret;
}

/*
 * Close the io_uring ring without submitting cancellations or running callbacks.
 *
 * Drain the loop first to close it cleanly, see poor_loop_drain().
 */
static inline void poor_loop_exit(struct poor_loop *loop)
{
	poor_loop_tsan_ring_release(&loop->ring);
	io_uring_queue_exit(&loop->ring);
}

/*
 * Run one iteration of the event loop, regardless of the stop flag.
 *
 * Callers driving their own loop can check poor_loop_stopped() between
 * iterations or use their own termination condition. This function does not
 * clear the flag.
 *
 * Callbacks must not call poor_loop_run(), poor_loop_run_once(),
 * poor_loop_drain() or poor_loop_exit().
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

[[nodiscard]] int poor_loop_drain_impl(struct poor_loop *loop, uint64_t deadline);

/*
 * Wait for untracked requests, such as closes, before poor_loop_exit().
 *
 * This is optional and only meant for shutting the loop down: call it once no
 * ops are pending and no timers are armed. 'deadline' is an absolute
 * CLOCK_MONOTONIC timestamp in nanoseconds (see poor_loop_now()).
 *
 * Returns 0, -ETIME if the deadline passed, -EINTR if a signal interrupted the
 * wait, -EBUSY if a timer is armed or an op completed, or another -errno on
 * error, such as -EOPNOTSUPP if the ring ever had a request with
 * IOSQE_CQE_SKIP_SUCCESS.
 *
 * IMPORTANT: If -EINTR or -ETIME is returned, only poor_loop_drain() or
 * poor_loop_exit() can be called, otherwise only poor_loop_exit().
 */
[[nodiscard]] static inline int poor_loop_drain(struct poor_loop *loop, uint64_t deadline)
{
	if (!(loop->ring.features & IORING_FEAT_EXT_ARG))
		return -EOPNOTSUPP;
	if (!poor_list_empty(&loop->timers))
		return -EBUSY;
	return poor_loop_drain_impl(loop, deadline);
}

#ifdef __cplusplus
}
#endif

#endif
