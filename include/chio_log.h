/* SPDX-License-Identifier: MIT */
#ifndef CHIO_LOG_H
#define CHIO_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

struct chio_loop;

typedef void chio_log_fn(struct chio_loop *loop, const char *fmt, va_list ap);

/* Set the calling thread's log function, or nullptr to disable logging. */
void chio_log_function_set(chio_log_fn *fn);

#ifdef __cplusplus
}
#endif

#endif
