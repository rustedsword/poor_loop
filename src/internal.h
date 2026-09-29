/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_INTERNAL_H
#define POOR_LOOP_INTERNAL_H

#include "poor_loop.h"

[[gnu::visibility("hidden"), gnu::format(printf, 2, 3)]] void
poor_loop_log(struct poor_loop *loop, const char *fmt, ...);
[[gnu::visibility("hidden")]] void
poor_loop_ops_dispatch(struct poor_loop *loop);
[[gnu::visibility("hidden")]] bool
poor_loop_timers_timeout(struct poor_loop *loop, struct __kernel_timespec *ts);
[[gnu::visibility("hidden")]] void
poor_loop_timers_expire(struct poor_loop *loop);

#endif
