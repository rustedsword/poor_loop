/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>

#include "chioloop.h"

static struct chio_timer *timer_of(struct chio_list *link)
{
	return chio_container_of(link, struct chio_timer, link);
}

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

struct io_uring_sqe *chio_get_sqe(struct chio_loop *loop, struct chio_op *op)
{
	struct io_uring_sqe *sqe;
	int ret;

	if (op->pending) {
		errno = EBUSY;
		return nullptr;
	}
	while (!(sqe = io_uring_get_sqe(&loop->ring))) {
		ret = io_uring_submit(&loop->ring);
		if (ret >= 0)
			ret = io_uring_sqring_wait(&loop->ring);
		if (ret < 0) {
			errno = -ret;
			return nullptr;
		}
	}
	io_uring_sqe_set_data(sqe, op);
	op->pending = true;
	return sqe;
}

void chio_timer_arm(struct chio_loop *loop, struct chio_timer *timer,
		    uint64_t deadline)
{
	struct chio_list *at = &loop->timers;

	chio_timer_disarm(timer);
	timer->deadline = deadline;
	if (!chio_list_empty(at) && timer_of(at->next)->deadline <= deadline) {
		at = at->prev;
		while (timer_of(at)->deadline > deadline)
			at = at->prev;
	}
	chio_list_insert(at, &timer->link);
}

static struct __kernel_timespec timeout(struct chio_loop *loop)
{
	uint64_t deadline = timer_of(loop->timers.next)->deadline;
	uint64_t now = chio_now();
	uint64_t left = deadline > now ? deadline - now : 0;

	return (struct __kernel_timespec){
		.tv_sec = left / 1'000'000'000,
		.tv_nsec = left % 1'000'000'000,
	};
}

static void dispatch(struct chio_loop *loop)
{
	unsigned budget = io_uring_cq_ready(&loop->ring);
	struct io_uring_cqe *cqe;
	struct chio_op *op;

	for (; budget && !io_uring_peek_cqe(&loop->ring, &cqe); budget--) {
		op = io_uring_cqe_get_data(cqe);
		if (op) {
			if (!(cqe->flags & IORING_CQE_F_MORE))
				op->pending = false;
			op->complete(loop, op, cqe);
		}
		io_uring_cqe_seen(&loop->ring, cqe);
	}
}

static void expire(struct chio_loop *loop)
{
	struct chio_timer *timer;
	struct chio_list due;
	uint64_t now;

	if (chio_list_empty(&loop->timers))
		return;
	now = chio_now();
	chio_list_init(&due);
	while (!chio_list_empty(&loop->timers)) {
		timer = timer_of(loop->timers.next);
		if (timer->deadline > now)
			break;
		chio_list_remove(&timer->link);
		chio_list_append(&due, &timer->link);
	}
	while (!chio_list_empty(&due)) {
		timer = timer_of(due.next);
		chio_list_remove(&timer->link);
		timer->fire(loop, timer);
	}
}

static int step(struct chio_loop *loop)
{
	struct __kernel_timespec ts;
	struct io_uring_cqe *cqe;
	int ret;

	if (chio_list_empty(&loop->timers)) {
		ret = io_uring_submit_and_wait(&loop->ring, 1);
	} else {
		ts = timeout(loop);
		ret = io_uring_submit_and_wait_timeout(&loop->ring, &cqe, 1,
						       &ts, nullptr);
	}
	if (ret == -EAGAIN || ret == -ENOMEM)
		ret = io_uring_get_events(&loop->ring);
	dispatch(loop);
	expire(loop);
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
