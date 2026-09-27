/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "internal.h"

static struct chio_timer *timer_of(struct chio_list *link)
{
	return chio_container_of(link, struct chio_timer, link);
}

void chio_timer_arm(struct chio_loop *loop, struct chio_timer *timer,
		    uint64_t deadline)
{
	struct chio_list *at = &loop->timers;

	chio_timer_disarm(timer);
	timer->deadline = deadline;
	if (!chio_list_empty(at) && timer_of(at->next)->deadline <= deadline) {
		at = at->prev;
		while (timer_of(at)->deadline > deadline)
			at = at->prev;
	}
	chio_list_insert(at, &timer->link);
}

bool chio_timers_timeout(struct chio_loop *loop, struct __kernel_timespec *ts)
{
	uint64_t deadline, now, left;

	if (chio_list_empty(&loop->timers))
		return false;
	deadline = timer_of(loop->timers.next)->deadline;
	now = chio_now();
	left = deadline > now ? deadline - now : 0;
	ts->tv_sec = left / 1'000'000'000;
	ts->tv_nsec = left % 1'000'000'000;
	return true;
}

void chio_timers_expire(struct chio_loop *loop)
{
	struct chio_timer *timer;
	struct chio_list due;
	uint64_t now;

	if (chio_list_empty(&loop->timers))
		return;
	now = chio_now();
	chio_list_init(&due);
	while (!chio_list_empty(&loop->timers)) {
		timer = timer_of(loop->timers.next);
		if (timer->deadline > now)
			break;
		chio_list_remove(&timer->link);
		chio_list_append(&due, &timer->link);
	}
	while (!chio_list_empty(&due)) {
		timer = timer_of(due.next);
		chio_list_remove(&timer->link);
		timer->fire(loop, timer);
	}
}
