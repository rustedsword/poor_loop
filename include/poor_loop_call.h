/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_CALL_H
#define POOR_LOOP_CALL_H

#ifdef __cplusplus
extern "C" {
#endif

struct poor_loop;
struct poor_loop_call;

typedef void poor_loop_call_fn(struct poor_loop *loop, struct poor_loop_call *call);

/*
 * Deferred call.
 *
 * Calls a callback once a bit later.
 *
 * It is safe to free struct poor_loop_call memory inside the callback.
 */
struct poor_loop_call {
	struct poor_loop_call *next;
	poor_loop_call_fn *fn;
};

#define POOR_LOOP_CALL_INIT(f) { .fn = (f) }

static inline void poor_loop_call_init(struct poor_loop_call *call, poor_loop_call_fn *fn)
{
	call->next = nullptr;
	call->fn = fn;
}

static inline bool poor_loop_call_queued(const struct poor_loop_call *call)
{
	return call->next != nullptr;
}

/* Queue a call. Queueing a queued call does nothing. */
void poor_loop_call_queue(struct poor_loop *loop, struct poor_loop_call *call);

/* Cancel a call. The callback will not be called. Cancelling twice is safe. */
void poor_loop_call_cancel(struct poor_loop *loop, struct poor_loop_call *call);

#ifdef __cplusplus
}
#endif

#endif
