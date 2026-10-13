/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "internal.h"

void poor_loop_call_queue(struct poor_loop *loop, struct poor_loop_call *call)
{
	struct poor_loop_call *last = loop->calls;

	if (poor_loop_call_queued(call))
		return;
	if (last) {
		call->next = last->next;
		last->next = call;
	} else {
		call->next = call;
	}
	loop->calls = call;
}

void poor_loop_call_cancel(struct poor_loop *loop, struct poor_loop_call *call)
{
	struct poor_loop_call *prev = call;

	if (!poor_loop_call_queued(call))
		return;
	while (prev->next != call)
		prev = prev->next;
	prev->next = call->next;
	call->next = nullptr;
	if (loop->calls == call)
		loop->calls = prev != call ? prev : nullptr;
}

void poor_loop_calls_run(struct poor_loop *loop)
{
	struct poor_loop_call end, *call;

	if (!loop->calls)
		return;
	end.next = loop->calls->next;
	loop->calls->next = &end;
	loop->calls = nullptr;
	while ((call = end.next) != &end) {
		end.next = call->next;
		call->next = nullptr;
		call->fn(loop, call);
	}
}
