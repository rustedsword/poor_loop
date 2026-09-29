/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "internal.h"

void poor_loop_timer_arm(struct poor_loop *loop, struct poor_loop_timer *timer, uint64_t deadline)
{
	struct poor_loop_timer *at;

	if (poor_loop_timer_armed(timer))
		poor_list_node_remove(&timer->link);
	timer->deadline = deadline;
	at = poor_list_first(&loop->timers);
	if (!at || at->deadline > deadline) {
		poor_list_prepend(&loop->timers, timer);
		return;
	}
	at = poor_list_last(&loop->timers);
	while (at->deadline > deadline)
		at = poor_list_prev(&loop->timers, at);
	poor_list_insert_after(&loop->timers, at, timer);
}

bool poor_loop_timers_timeout(struct poor_loop *loop, struct __kernel_timespec *ts)
{
	struct poor_loop_timer *first = poor_list_first(&loop->timers);
	uint64_t deadline, now, left;

	if (!first)
		return false;
	deadline = first->deadline;
	now = poor_loop_now();
	left = deadline > now ? deadline - now : 0;
	ts->tv_sec = left / 1'000'000'000;
	ts->tv_nsec = left % 1'000'000'000;
	return true;
}

void poor_loop_timers_expire(struct poor_loop *loop)
{
	poor_loop_timer_list due = POOR_LIST_INIT(due);
	struct poor_loop_timer *timer;
	uint64_t now;

	if (poor_list_empty(&loop->timers))
		return;
	now = poor_loop_now();
	while ((timer = poor_list_first(&loop->timers))) {
		if (timer->deadline > now)
			break;
		poor_list_remove(&loop->timers, timer);
		poor_list_append(&due, timer);
	}
	while ((timer = poor_list_first(&due))) {
		poor_loop_timer_disarm(timer);
		timer->fire(loop, timer);
	}
}
