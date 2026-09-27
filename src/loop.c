/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>

#include "internal.h"

int chio_loop_init(struct chio_loop *loop, unsigned entries,
		   struct io_uring_params *params)
{
	chio_list_init(&loop->timers);
	loop->stop = false;
	return io_uring_queue_init_params(entries, &loop->ring, params);
}

void chio_loop_exit(struct chio_loop *loop)
{
	io_uring_queue_exit(&loop->ring);
}

static int step(struct chio_loop *loop)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	int ret;

	if (chio_timers_timeout(loop, &ts))
		ret = io_uring_submit_and_wait_timeout(&loop->ring, &cqe, 1,
						       &ts, nullptr);
	else
		ret = io_uring_submit_and_wait(&loop->ring, 1);
	if (ret == -EAGAIN || ret == -ENOMEM)
		ret = io_uring_get_events(&loop->ring);
	chio_ops_dispatch(loop);
	chio_timers_expire(loop);
	if (ret == -EINTR || ret == -EBUSY || ret == -ETIME)
		return 0;
	return ret < 0 ? ret : 0;
}

int chio_loop_run(struct chio_loop *loop)
{
	int ret = 0;

	while (!loop->stop && !ret)
		ret = step(loop);
	loop->stop = false;
	return ret;
}

void chio_loop_stop(struct chio_loop *loop)
{
	loop->stop = true;
}
