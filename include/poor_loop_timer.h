/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_TIMER_H
#define POOR_LOOP_TIMER_H

#include <poor_list.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct poor_loop;
struct poor_loop_timer;

typedef void poor_loop_timer_fn(struct poor_loop *loop, struct poor_loop_timer *timer);

/*
 * Software timer.
 *
 * 'deadline' is an absolute CLOCK_MONOTONIC timestamp in nanoseconds (see
 * poor_loop_now()). The timer is automatically disarmed before fire() runs, so the
 * callback is free to re-arm it.
 *
 * An armed timer must be disarmed before it is freed.
 */
struct poor_loop_timer {
	struct poor_list_node link;
	uint64_t deadline;
	poor_loop_timer_fn *fire;
};

poor_list_define(poor_loop_timer_list, struct poor_loop_timer, link);

#define POOR_LOOP_TIMER_INIT(fn) { .fire = (fn) }

static inline void poor_loop_timer_init(struct poor_loop_timer *timer, poor_loop_timer_fn *fire)
{
	timer->link.next = nullptr;
	timer->fire = fire;
}

static inline bool poor_loop_timer_armed(const struct poor_loop_timer *timer)
{
	return timer->link.next != nullptr;
}

static inline void poor_loop_timer_disarm(struct poor_loop_timer *timer)
{
	if (poor_loop_timer_armed(timer)) {
		poor_list_node_remove(&timer->link);
		timer->link.next = nullptr;
	}
}

static inline uint64_t poor_loop_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1'000'000'000 + (uint64_t)ts.tv_nsec;
}

/*
 * Arm or re-arm a timer. Timers with equal deadlines fire in the order they
 * were armed.
 */
void poor_loop_timer_arm(struct poor_loop *loop, struct poor_loop_timer *timer, uint64_t deadline);

#ifdef __cplusplus
}
#endif

#endif
