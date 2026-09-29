/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "internal.h"

static thread_local poor_loop_log_fn *log_fn;

void poor_loop_log_function_set(poor_loop_log_fn *fn)
{
	log_fn = fn;
}

void poor_loop_log(struct poor_loop *loop, const char *fmt, ...)
{
	va_list ap;

	if (!log_fn)
		return;
	va_start(ap, fmt);
	log_fn(loop, fmt, ap);
	va_end(ap);
}
