/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_LOG_H
#define POOR_LOOP_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

struct poor_loop;

typedef void poor_loop_log_fn(struct poor_loop *loop, const char *fmt, va_list ap);

/* Set the calling thread's log function, or nullptr to disable logging. */
void poor_loop_log_function_set(poor_loop_log_fn *fn);

#ifdef __cplusplus
}
#endif

#endif
