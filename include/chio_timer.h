/* SPDX-License-Identifier: MIT */
#ifndef CHIO_TIMER_H
#define CHIO_TIMER_H

#include <poor_list.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

struct chio_loop;
struct chio_timer;

typedef void chio_timer_fn(struct chio_loop *loop, struct chio_timer *timer);

/*
 * Software timer.
 *
 * 'deadline' is an absolute CLOCK_MONOTONIC timestamp in nanoseconds (see
 * chio_now()). The timer is automatically disarmed before fire() runs, so the
 * callback is free to re-arm it.
 *
 * An armed timer must be disarmed before it is freed.
 */
struct chio_timer {
	struct poor_list_node link;
	uint64_t deadline;
	chio_timer_fn *fire;
};

poor_list_define(chio_timer_list, struct chio_timer, link);

#define CHIO_TIMER_INIT(fn) { .link = {}, .deadline = 0, .fire = (fn) }

static inline void chio_timer_init(struct chio_timer *timer,
				   chio_timer_fn *fire)
{
	struct chio_timer init = CHIO_TIMER_INIT(fire);

	*timer = init;
}

static inline bool chio_timer_armed(const struct chio_timer *timer)
{
	return timer->link.next != nullptr;
}

static inline void chio_timer_disarm(struct chio_timer *timer)
{
	if (chio_timer_armed(timer)) {
		poor_list_node_remove(&timer->link);
		timer->link.next = nullptr;
	}
}

static inline uint64_t chio_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1'000'000'000 + (uint64_t)ts.tv_nsec;
}

/*
 * Arm or re-arm a timer. Timers with equal deadlines fire in the order they
 * were armed.
 */
void chio_timer_arm(struct chio_loop *loop, struct chio_timer *timer,
		    uint64_t deadline);

#ifdef __cplusplus
}
#endif

#endif
