/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>

#include "internal.h"

int poor_loop_init(struct poor_loop *loop, unsigned entries, struct io_uring_params *params)
{
	poor_list_init(&loop->timers);
	loop->stop = false;
	return io_uring_queue_init_params(entries, &loop->ring, params);
}

int poor_loop_run_once(struct poor_loop *loop)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	int ret;

	if (poor_loop_timers_timeout(loop, &ts))
		ret = io_uring_submit_and_wait_timeout(&loop->ring, &cqe, 1, &ts, nullptr);
	else
		ret = io_uring_submit_and_wait(&loop->ring, 1);
	if (ret == -EAGAIN || ret == -ENOMEM)
		ret = io_uring_get_events(&loop->ring);
	poor_loop_ops_dispatch(loop);
	poor_loop_timers_expire(loop);
	if (ret == -EINTR || ret == -EBUSY || ret == -ETIME)
		return 0;
	return ret < 0 ? ret : 0;
}
