/* SPDX-License-Identifier: MIT */
#ifndef CHIO_INTERNAL_H
#define CHIO_INTERNAL_H

#include "chioloop.h"

[[gnu::visibility("hidden")]] void chio_ops_dispatch(struct chio_loop *loop);
[[gnu::visibility("hidden")]] bool
chio_timers_timeout(struct chio_loop *loop, struct __kernel_timespec *ts);
[[gnu::visibility("hidden")]] void chio_timers_expire(struct chio_loop *loop);

#endif
