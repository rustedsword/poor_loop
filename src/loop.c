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

int poor_loop_drain_impl(struct poor_loop *loop, uint64_t deadline)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	int ret;

	/* End a link chain the queued SQEs may leave open, or the NOP would join it */
	auto sqe = poor_loop_get_untracked_sqe_or_submit(loop);
	if (!sqe)
		return -errno;
	io_uring_prep_nop(sqe);
	sqe = poor_loop_get_untracked_sqe_or_submit(loop);
	if (!sqe)
		return -errno;
	io_uring_prep_nop(sqe);
	io_uring_sqe_set_data(sqe, loop);
	sqe->flags |= IOSQE_IO_DRAIN;
	for (;;) {
		auto now = poor_loop_now();
		auto left = deadline > now ? deadline - now : 0;

		ts.tv_sec = left / 1'000'000'000;
		ts.tv_nsec = left % 1'000'000'000;
		ret = io_uring_submit_and_wait_timeout(&loop->ring, &cqe, 1, &ts, nullptr);
		if (ret == -EAGAIN || ret == -ENOMEM)
			ret = io_uring_get_events(&loop->ring);
		auto budget = io_uring_cq_ready(&loop->ring);
		for (; budget && !io_uring_peek_cqe(&loop->ring, &cqe); budget--) {
			void *data = io_uring_cqe_get_data(cqe);
			int res = cqe->res;

			io_uring_cqe_seen(&loop->ring, cqe);
			if (data == loop)
				return res;
			if (data) {
				poor_loop_log(loop, "drain: unexpected CQE for op ", data, ", res ", res);
				return -EBUSY;
			}
		}
		if (ret < 0 && ret != -EBUSY && ret != -ETIME)
			return ret;
		if (!left)
			return -ETIME;
	}
}
