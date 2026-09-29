/* SPDX-License-Identifier: MIT */
#ifndef CHIO_LOOP_H
#define CHIO_LOOP_H

#include <liburing.h>

#include "chio_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

struct chio_loop {
	struct io_uring ring;
	chio_timer_list timers;
	bool stop;
};

/*
 * Return the ring for direct use with liburing.
 *
 * The loop owns the CQ, so never consume CQEs.
 */
static inline struct io_uring *chio_loop_ring(struct chio_loop *loop)
{
	return &loop->ring;
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
 * Disarm all timers and make sure no op is pending first: cancel requests that
 * won't finish on their own and run the loop until they complete. The kernel
 * cancels whatever is left only after the ring is closed, without calling
 * complete(): a running request may still use its buffers after this returns,
 * and a queued close may leave its fd open.
 */
void chio_loop_exit(struct chio_loop *loop);

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
