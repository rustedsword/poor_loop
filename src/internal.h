/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_INTERNAL_H
#define POOR_LOOP_INTERNAL_H

#include <poor_stdio.h>

#include "poor_loop.h"

#define poor_loop_log(loop, ...) poor_loop_logf(loop, printf_specifier_string(0, __VA_ARGS__), printf_args_pre_process(__VA_ARGS__))

[[gnu::visibility("hidden"), gnu::format(printf, 2, 3)]] void poor_loop_logf(struct poor_loop *loop, const char *fmt, ...);
[[gnu::visibility("hidden")]] void poor_loop_ops_dispatch(struct poor_loop *loop);
[[gnu::visibility("hidden")]] bool poor_loop_timers_timeout(struct poor_loop *loop, struct __kernel_timespec *ts);
[[gnu::visibility("hidden")]] void poor_loop_timers_expire(struct poor_loop *loop);

#endif
