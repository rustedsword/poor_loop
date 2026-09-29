/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "internal.h"

void chio_timer_arm(struct chio_loop *loop, struct chio_timer *timer,
		    uint64_t deadline)
{
	struct chio_timer *at;

	if (chio_timer_armed(timer))
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

bool chio_timers_timeout(struct chio_loop *loop, struct __kernel_timespec *ts)
{
	struct chio_timer *first = poor_list_first(&loop->timers);
	uint64_t deadline, now, left;

	if (!first)
		return false;
	deadline = first->deadline;
	now = chio_now();
	left = deadline > now ? deadline - now : 0;
	ts->tv_sec = left / 1'000'000'000;
	ts->tv_nsec = left % 1'000'000'000;
	return true;
}

void chio_timers_expire(struct chio_loop *loop)
{
	chio_timer_list due = POOR_LIST_INIT(due);
	struct chio_timer *timer;
	uint64_t now;

	if (poor_list_empty(&loop->timers))
		return;
	now = chio_now();
	while ((timer = poor_list_first(&loop->timers))) {
		if (timer->deadline > now)
			break;
		poor_list_remove(&loop->timers, timer);
		poor_list_append(&due, timer);
	}
	while ((timer = poor_list_first(&due))) {
		chio_timer_disarm(timer);
		timer->fire(loop, timer);
	}
}
