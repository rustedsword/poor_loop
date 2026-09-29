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
 * Tear down the event loop and close the io_uring ring.
 *
 * Disarm all timers and make sure no op is pending first: cancel requests that
 * won't finish on their own and run the loop until they complete. The kernel
 * cancels whatever is left only after the ring is closed, without calling
 * complete(): a running request may still use its buffers after this returns,
 * and a queued close may leave its fd open.
 */
void poor_loop_exit(struct poor_loop *loop);

/*
 * Run the event loop until poor_loop_stop() is called.
 *
 * Callbacks must not call poor_loop_run() or poor_loop_exit(). CQEs with zero
 * user_data are ignored.
 *
 * Returns 0 on normal exit, or -errno if waiting failed.
 */
[[nodiscard]] int poor_loop_run(struct poor_loop *loop);

/*
 * Stop the event loop.
 *
 * If called from a callback, poor_loop_run() will exit after the current
 * iteration finishes.
 */
void poor_loop_stop(struct poor_loop *loop);

#ifdef __cplusplus
}
#endif

#endif
