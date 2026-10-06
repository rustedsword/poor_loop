/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poll.h>

#include "harness.h"
#include "shared_group.h"

poor_loop_group_declare(held_work);

struct held {
	held_work work;
	struct poor_loop_op op;
	struct poor_loop_timer timer;
	unsigned completed, calls;
};

poor_loop_group_attach(held_work, struct held, work, op, held_op_done);
poor_loop_group_attach(held_work, struct held, work, timer, held_fire);

POOR_LOOP_GROUP_OP(held_op_done, loop, work, op, cqe)
{
	CHECK_EQ(cqe->res, 0);
	container_of(work, struct held, work)->completed++;
}

POOR_LOOP_GROUP_TIMER(held_fire, loop, work, timer)
{
	CHECK(false);
}

POOR_LOOP_GROUP_DONE(held_work, loop, work)
{
	CHECK_EQ(work->group.pending, 0);
	container_of(work, struct held, work)->calls++;
	poor_loop_stop(loop);
}

static int test_group_empty(void)
{
	static struct held h = { .work = POOR_LOOP_GROUP_INIT };

	CHECK_EQ(h.work.group.pending, 0);
	memset(&h.work, 0xa5, sizeof(h.work));
	poor_loop_group_init(&h.work);
	CHECK_EQ(h.work.group.pending, 0);
	for (unsigned i = 0; i < 2; i++) {
		poor_loop_group_acquire(&h.work);
		CHECK(poor_loop_group_release(&h.work));
	}
	CHECK_EQ(h.calls, 0);
	return 0;
}

static int test_group_holds(void)
{
	struct held h = { .work = POOR_LOOP_GROUP_INIT };

	for (unsigned i = 0; i < 64; i++)
		poor_loop_group_acquire(&h.work);
	for (unsigned i = 1; i < 64; i++)
		CHECK(!poor_loop_group_release(&h.work));
	CHECK(poor_loop_group_release(&h.work));
	CHECK_EQ(h.calls, 0);
	return 0;
}

static int test_group_release_pending(void)
{
	struct held h = {
		.work = POOR_LOOP_GROUP_INIT,
		.op = POOR_LOOP_OP_INIT(held_op_done),
	};
	struct poor_loop loop;

	loop_init(&loop, 8);
	poor_loop_group_acquire(&h.work);
	io_uring_prep_nop(poor_loop_group_get_sqe(&loop, &h.work, &h.op));
	CHECK(!poor_loop_group_release(&h.work));
	CHECK_EQ(h.calls, 0);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(h.completed, 1);
	CHECK_EQ(h.calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

static int test_group_timer_disarm(void)
{
	struct held h = {
		.work = POOR_LOOP_GROUP_INIT,
		.op = POOR_LOOP_OP_INIT(held_op_done),
		.timer = POOR_LOOP_TIMER_INIT(held_fire),
	};
	uint64_t later = poor_loop_now() + 10'000'000'000;
	struct poor_loop loop;

	loop_init(&loop, 8);
	CHECK(!poor_loop_group_timer_disarm(&h.work, &h.timer));
	poor_loop_group_timer_arm(&loop, &h.work, &h.timer, later);
	CHECK(poor_loop_group_timer_disarm(&h.work, &h.timer));
	CHECK(!poor_loop_group_timer_disarm(&h.work, &h.timer));
	CHECK(!poor_loop_timer_armed(&h.timer));
	CHECK(poor_list_empty(&loop.timers));
	CHECK_EQ(h.work.group.pending, 0);
	CHECK_EQ(h.calls, 0);
	poor_loop_group_call_done(held_work, &loop, &h.work);
	CHECK_EQ(h.calls, 1);
	poor_loop_clear_stop(&loop);
	poor_loop_group_timer_arm(&loop, &h.work, &h.timer, later);
	io_uring_prep_nop(poor_loop_group_get_sqe(&loop, &h.work, &h.op));
	CHECK(!poor_loop_group_timer_disarm(&h.work, &h.timer));
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(h.completed, 1);
	CHECK_EQ(h.calls, 2);
	poor_loop_exit(&loop);
	return 0;
}

poor_loop_group_declare(one_work);

struct one {
	one_work work;
	struct poor_loop_op first, second;
	unsigned completed, calls;
};

poor_loop_group_attach(one_work, struct one, work, first, one_done);
poor_loop_group_attach(one_work, struct one, work, second, one_second_done);

POOR_LOOP_GROUP_OP(one_done, loop, work, op, cqe)
{
	struct one *o = container_of(work, struct one, work);

	CHECK(op == &o->first);
	CHECK_EQ(cqe->res, 0);
	CHECK_EQ(o->calls, 0);
	CHECK_EQ(work->group.pending, 1);
	o->completed++;
}

POOR_LOOP_GROUP_OP(one_second_done, loop, work, op, cqe)
{
	CHECK(false);
}

POOR_LOOP_GROUP_DONE(one_work, loop, work)
{
	struct one *o = container_of(work, struct one, work);

	CHECK_EQ(o->completed, 1);
	o->calls++;
}

static int test_group_sqe_failure(void)
{
	struct one o = {
		.first = POOR_LOOP_OP_INIT(one_done),
		.second = POOR_LOOP_OP_INIT(one_second_done),
	};
	struct io_uring_params p;
	struct poor_loop loop;

	setup_params(&p);
	p.flags |= IORING_SETUP_R_DISABLED;
	CHECK_EQ(poor_loop_init(&loop, 2, &p), 0);
	poor_loop_group_init(&o.work);
	io_uring_prep_nop(poor_loop_group_get_sqe(&loop, &o.work, &o.first));
	CHECK(!poor_loop_group_get_sqe(&loop, &o.work, &o.first));
	CHECK_EQ(errno, EBUSY);
	io_uring_prep_nop(poor_loop_get_untracked_sqe(&loop));
	CHECK(!poor_loop_group_get_sqe(&loop, &o.work, &o.second));
	CHECK_EQ(errno, EAGAIN);
	CHECK(!poor_loop_group_get_sqe_or_submit(&loop, &o.work, &o.second));
	CHECK_EQ(errno, EBADFD);
	CHECK(!o.second.pending);
	CHECK_EQ(o.work.group.pending, 1);
	enable_ring(&loop);
	while (!o.calls)
		CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(o.calls, 1);
	CHECK_EQ(io_uring_sq_ready(poor_loop_ring(&loop)), 0);
	poor_loop_exit(&loop);
	return 0;
}

static int test_group_after_callback(void)
{
	struct one o = {
		.work = POOR_LOOP_GROUP_INIT,
		.first = POOR_LOOP_OP_INIT(one_done),
		.second = POOR_LOOP_OP_INIT(one_second_done),
	};
	struct poor_loop loop;

	loop_init(&loop, 8);
	io_uring_prep_nop(poor_loop_group_get_sqe(&loop, &o.work, &o.first));
	while (!o.calls)
		CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(o.calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

poor_loop_group_declare(reads_work);

struct reads {
	reads_work work;
	struct poor_loop_op first, second, error, close;
	int fd;
	unsigned completed;
	bool closed;
	char buf[2];
};

poor_loop_group_attach(reads_work, struct reads, work, first, read_done);
poor_loop_group_attach(reads_work, struct reads, work, second, second_done);
poor_loop_group_attach(reads_work, struct reads, work, error, error_done);

POOR_LOOP_GROUP_OP(read_done, loop, work, op, cqe)
{
	CHECK_EQ(cqe->res, 1);
	container_of(work, struct reads, work)->completed++;
}

POOR_LOOP_GROUP_OP(second_done, loop, work, op, cqe)
{
	CHECK_EQ(cqe->res, 1);
	container_of(work, struct reads, work)->completed++;
}

POOR_LOOP_GROUP_OP(error_done, loop, work, op, cqe)
{
	CHECK_EQ(cqe->res, -EBADF);
	container_of(work, struct reads, work)->completed++;
}

static void close_done(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct reads *r = container_of(op, struct reads, close);

	CHECK_EQ(cqe->res, 0);
	CHECK_EQ(fcntl(r->fd, F_GETFD), -1);
	CHECK_EQ(errno, EBADF);
	r->closed = true;
	poor_loop_stop(loop);
}

POOR_LOOP_GROUP_DONE(reads_work, loop, work)
{
	struct reads *r = container_of(work, struct reads, work);
	struct io_uring_sqe *sqe;

	CHECK_EQ(r->completed, 3);
	CHECK(!r->first.pending && !r->second.pending && !r->error.pending);
	CHECK(fcntl(r->fd, F_GETFD) >= 0);
	sqe = get_sqe(loop, &r->close);
	io_uring_prep_close(sqe, r->fd);
	sqe->flags |= IOSQE_ASYNC;
}

static int test_group_reads_close(void)
{
	struct reads r = {
		.work = POOR_LOOP_GROUP_INIT,
		.first = POOR_LOOP_OP_INIT(read_done),
		.second = POOR_LOOP_OP_INIT(second_done),
		.error = POOR_LOOP_OP_INIT(error_done),
		.close = POOR_LOOP_OP_INIT(close_done),
	};
	struct poor_loop loop;
	int fds[2];

	make_pipe(&fds);
	r.fd = fds[0];
	CHECK_EQ(write(fds[1], "ab", 2), 2);
	/* Queuing more work than fits may submit, but must not dispatch callbacks. */
	loop_init(&loop, 2);
	io_uring_prep_read_array(poor_loop_group_get_sqe_or_submit(&loop, &r.work, &r.first), r.fd, array_ptr(&r.buf[0]), 0);
	io_uring_prep_read_array(poor_loop_group_get_sqe_or_submit(&loop, &r.work, &r.second), r.fd, array_ptr(&r.buf[1]), 0);
	io_uring_prep_read(poor_loop_group_get_sqe_or_submit(&loop, &r.work, &r.error), -1, nullptr, 1, 0);
	CHECK_EQ(r.work.group.pending, 3);
	CHECK_EQ(r.completed, 0);
	CHECK(!r.closed);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK(r.closed);
	CHECK((r.buf[0] == 'a' && r.buf[1] == 'b') || (r.buf[0] == 'b' && r.buf[1] == 'a'));
	poor_loop_exit(&loop);
	CHECK_EQ(close(fds[1]), 0);
	return 0;
}

poor_loop_group_declare(phases_work);

struct phases {
	phases_work work;
	struct poor_loop_op op;
	unsigned completed, phases;
};

poor_loop_group_attach(phases_work, struct phases, work, op, phase_op_done);

static void phase_start(struct poor_loop *loop, struct phases *p)
{
	io_uring_prep_nop(poor_loop_group_get_sqe(loop, &p->work, &p->op));
}

POOR_LOOP_GROUP_OP(phase_op_done, loop, work, op, cqe)
{
	struct phases *p = container_of(work, struct phases, work);

	CHECK_EQ(cqe->res, 0);
	if (++p->completed % 3)
		phase_start(loop, p);
}

POOR_LOOP_GROUP_DONE(phases_work, loop, work)
{
	struct phases *p = container_of(work, struct phases, work);

	CHECK_EQ(p->completed, 3 * ++p->phases);
	if (p->phases == 3) {
		poor_loop_stop(loop);
		return;
	}
	phase_start(loop, p);
}

static int test_group_phases(void)
{
	struct phases p = {
		.work = POOR_LOOP_GROUP_INIT,
		.op = POOR_LOOP_OP_INIT(phase_op_done),
	};
	struct poor_loop loop;

	loop_init(&loop, 8);
	phase_start(&loop, &p);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(p.phases, 3);
	CHECK_EQ(p.completed, 9);
	poor_loop_exit(&loop);
	return 0;
}

poor_loop_group_declare(watched_work);

struct watched {
	watched_work work;
	struct poor_loop_op poll, cancel;
	bool *freed;
	unsigned events;
	bool cancelled, cancel_completed;
};

poor_loop_group_attach(watched_work, struct watched, work, poll, poll_done);
poor_loop_group_attach(watched_work, struct watched, work, cancel, cancel_done);

POOR_LOOP_GROUP_DONE(watched_work, loop, work)
{
	struct watched *w = container_of(work, struct watched, work);
	bool *freed = w->freed;

	CHECK(w->events > 0);
	CHECK(w->cancelled);
	CHECK(w->cancel_completed);
	CHECK(!w->poll.pending && !w->cancel.pending);
	memset(w, 0xa5, sizeof(*w));
	free(w);
	*freed = true;
	poor_loop_stop(loop);
}

POOR_LOOP_GROUP_OP(poll_done, loop, work, op, cqe)
{
	struct watched *w = container_of(work, struct watched, work);

	if (cqe->flags & IORING_CQE_F_MORE) {
		CHECK(cqe->res & POLLIN);
		w->events++;
		poor_loop_stop(loop);
		return;
	}
	CHECK_EQ(cqe->res, -ECANCELED);
	w->cancelled = true;
}

POOR_LOOP_GROUP_OP(cancel_done, loop, work, op, cqe)
{
	CHECK_EQ(cqe->res, 0);
	container_of(work, struct watched, work)->cancel_completed = true;
}

static int test_group_multishot_free(void)
{
	struct watched *w = malloc(sizeof(*w));
	struct poor_loop loop;
	bool freed = false;
	int fds[2];
	char byte;

	CHECK(w);
	*w = (struct watched){
		.work = POOR_LOOP_GROUP_INIT,
		.poll = POOR_LOOP_OP_INIT(poll_done),
		.cancel = POOR_LOOP_OP_INIT(cancel_done),
		.freed = &freed,
	};
	make_pipe(&fds);
	loop_init(&loop, 8);
	io_uring_prep_poll_multishot(poor_loop_group_get_sqe(&loop, &w->work, &w->poll), fds[0], POLLIN);
	CHECK_EQ(write(fds[1], "x", 1), 1);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK(!freed);
	CHECK(w->poll.pending);
	CHECK(w->events > 0);
	CHECK_EQ(read(fds[0], &byte, 1), 1);
	io_uring_prep_cancel(poor_loop_group_get_sqe(&loop, &w->work, &w->cancel), &w->poll, 0);
	while (!freed)
		CHECK_EQ(poor_loop_run_once(&loop), 0);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

poor_loop_group_declare(ticker_work);

struct ticker {
	ticker_work work;
	struct poor_loop_timer first, second;
	unsigned fired, calls;
};

poor_loop_group_attach(ticker_work, struct ticker, work, first, ticker_fire);
poor_loop_group_attach(ticker_work, struct ticker, work, second, ticker_second_fire);

POOR_LOOP_GROUP_TIMER(ticker_fire, loop, work, timer)
{
	struct ticker *t = container_of(work, struct ticker, work);

	CHECK(timer == &t->first);
	CHECK_EQ(t->calls, 0);
	if (++t->fired == 1)
		poor_loop_group_timer_arm(loop, work, timer, poor_loop_now());
}

POOR_LOOP_GROUP_TIMER(ticker_second_fire, loop, work, timer)
{
	CHECK(false);
}

POOR_LOOP_GROUP_DONE(ticker_work, loop, work)
{
	struct ticker *t = container_of(work, struct ticker, work);

	CHECK_EQ(t->fired, 2);
	CHECK(!poor_loop_timer_armed(&t->first));
	CHECK(!poor_loop_timer_armed(&t->second));
	t->calls++;
	poor_loop_stop(loop);
}

static int test_group_timer(void)
{
	struct ticker t = {
		.work = POOR_LOOP_GROUP_INIT,
		.first = POOR_LOOP_TIMER_INIT(ticker_fire),
		.second = POOR_LOOP_TIMER_INIT(ticker_second_fire),
	};
	struct poor_loop loop;
	uint64_t later = poor_loop_now() + 10'000'000'000;

	loop_init(&loop, 8);
	poor_loop_group_timer_arm(&loop, &t.work, &t.first, later);
	poor_loop_group_timer_arm(&loop, &t.work, &t.first, 0);
	poor_loop_group_timer_arm(&loop, &t.work, &t.second, later);
	CHECK_EQ(t.work.group.pending, 2);
	CHECK(!poor_loop_group_timer_disarm(&t.work, &t.second));
	CHECK(!poor_loop_group_timer_disarm(&t.work, &t.second));
	CHECK_EQ(t.work.group.pending, 1);
	CHECK_EQ(t.calls, 0);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(t.calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

poor_loop_group_declare(pair_work);

struct pair {
	pair_work work;
	struct poor_loop_timer first, second;
	unsigned fired, calls;
};

poor_loop_group_attach(pair_work, struct pair, work, first, pair_first_fire);
poor_loop_group_attach(pair_work, struct pair, work, second, pair_second_fire);

POOR_LOOP_GROUP_TIMER(pair_first_fire, loop, work, timer)
{
	struct pair *p = container_of(work, struct pair, work);

	p->fired++;
	CHECK(!poor_loop_group_timer_disarm(work, &p->second));
	CHECK_EQ(work->group.pending, 1);
	CHECK_EQ(p->calls, 0);
}

POOR_LOOP_GROUP_TIMER(pair_second_fire, loop, work, timer)
{
	CHECK(false);
}

POOR_LOOP_GROUP_DONE(pair_work, loop, work)
{
	struct pair *p = container_of(work, struct pair, work);

	CHECK_EQ(p->fired, 1);
	CHECK(!poor_loop_timer_armed(&p->second));
	p->calls++;
	poor_loop_stop(loop);
}

static int test_group_disarm_in_callback(void)
{
	uint64_t now = poor_loop_now();

	/* The second timer is either due in the same pass or far away. */
	for (unsigned i = 0; i < 2; i++) {
		struct pair p = {
			.work = POOR_LOOP_GROUP_INIT,
			.first = POOR_LOOP_TIMER_INIT(pair_first_fire),
			.second = POOR_LOOP_TIMER_INIT(pair_second_fire),
		};
		struct poor_loop loop;

		loop_init(&loop, 8);
		poor_loop_group_timer_arm(&loop, &p.work, &p.first, now);
		poor_loop_group_timer_arm(&loop, &p.work, &p.second, i ? now + 10'000'000'000 : now);
		CHECK_EQ(poor_loop_run(&loop), 0);
		CHECK_EQ(p.calls, 1);
		poor_loop_exit(&loop);
	}
	return 0;
}

poor_loop_group_declare(layout_io);
poor_loop_group_declare(layout_clock);

struct layout {
	unsigned tag;
	layout_io io;
	struct poor_loop_op op;
	struct poor_loop_timer io_timer;
	layout_clock clock;
	struct poor_loop_timer timer;
	unsigned ops, io_ticks, ticks, completed;
};

static_assert(sizeof(struct poor_loop_group) == sizeof(unsigned));
static_assert(sizeof(layout_io) == sizeof(struct poor_loop_group));

poor_loop_group_attach(layout_io, struct layout, io, op, layout_op_done);
poor_loop_group_attach(layout_io, struct layout, io, io_timer, layout_io_timer_done);
poor_loop_group_attach(layout_clock, struct layout, clock, timer, layout_timer_done);

POOR_LOOP_GROUP_OP(layout_op_done, loop, io, op, cqe)
{
	struct layout *owner = container_of(io, struct layout, io);

	CHECK(op == &owner->op);
	CHECK_EQ(owner->tag, 123);
	CHECK_EQ(cqe->res, 0);
	owner->ops++;
}

POOR_LOOP_GROUP_TIMER(layout_io_timer_done, loop, io, timer)
{
	struct layout *owner = container_of(io, struct layout, io);

	CHECK(timer == &owner->io_timer);
	CHECK_EQ(owner->tag, 123);
	owner->io_ticks++;
}

POOR_LOOP_GROUP_TIMER(layout_timer_done, loop, clk, timer)
{
	struct layout *owner = container_of(clk, struct layout, clock);

	CHECK(timer == &owner->timer);
	CHECK_EQ(owner->tag, 123);
	owner->ticks++;
}

POOR_LOOP_GROUP_DONE(layout_io, loop, io)
{
	struct layout *owner = container_of(io, struct layout, io);

	CHECK_EQ(owner->ops, 1);
	CHECK_EQ(owner->io_ticks, 1);
	owner->completed++;
}

POOR_LOOP_GROUP_DONE(layout_clock, loop, clk)
{
	struct layout *owner = container_of(clk, struct layout, clock);

	CHECK_EQ(owner->ticks, 1);
	owner->completed++;
}

static int test_group_layout(void)
{
	struct layout owner = {
		.tag = 123,
		.io = POOR_LOOP_GROUP_INIT,
		.op = POOR_LOOP_OP_INIT(layout_op_done),
		.io_timer = POOR_LOOP_TIMER_INIT(layout_io_timer_done),
		.clock = POOR_LOOP_GROUP_INIT,
		.timer = POOR_LOOP_TIMER_INIT(layout_timer_done),
	};
	struct poor_loop loop;
	unsigned groups = 0, members = 0;

	loop_init(&loop, 8);
	io_uring_prep_nop(poor_loop_group_get_sqe(&loop, (groups++, &owner.io), (members++, &owner.op)));
	poor_loop_group_timer_arm(&loop, (groups++, &owner.io), (members++, &owner.io_timer), poor_loop_now());
	CHECK_EQ(groups, 2);
	CHECK_EQ(members, 2);
	/* Replacing the only armed timer does not complete the group. */
	poor_loop_group_timer_arm(&loop, &owner.clock, &owner.timer, poor_loop_now());
	poor_loop_group_timer_arm(&loop, &owner.clock, &owner.timer, poor_loop_now());
	CHECK_EQ(owner.clock.group.pending, 1);
	while (owner.completed < 2)
		CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(owner.completed, 2);
	poor_loop_exit(&loop);
	return 0;
}

poor_loop_group_declare(attached_work);

struct attached {
	struct poor_loop_op before;
	attached_work work;
	struct poor_loop_op after;
	unsigned completed, calls;
};

struct request {
	struct poor_loop_op op;
	attached_work *work;
};

struct alarm {
	struct poor_loop_timer timer;
	attached_work *work;
};

poor_loop_group_attach(attached_work, struct attached, work, before, attached_before_done);
poor_loop_group_attach(attached_work, struct attached, work, after, attached_after_done);
poor_loop_group_attach(attached_work, struct request, work, op, request_done);
poor_loop_group_attach(attached_work, struct alarm, work, timer, alarm_fire);

POOR_LOOP_GROUP_OP(attached_before_done, loop, work, op, cqe)
{
	struct attached *a = container_of(work, struct attached, work);

	CHECK(op == &a->before);
	CHECK_EQ(cqe->res, 0);
	a->completed++;
}

POOR_LOOP_GROUP_OP(attached_after_done, loop, work, op, cqe)
{
	struct attached *a = container_of(work, struct attached, work);

	CHECK(op == &a->after);
	CHECK_EQ(cqe->res, 0);
	a->completed++;
}

POOR_LOOP_GROUP_OP(request_done, loop, work, op, cqe)
{
	struct request *req = container_of(op, struct request, op);

	CHECK(work == req->work);
	CHECK_EQ(cqe->res, 0);
	container_of(work, struct attached, work)->completed++;
	memset(req, 0xa5, sizeof(*req));
	free(req);
}

POOR_LOOP_GROUP_TIMER(alarm_fire, loop, work, timer)
{
	struct alarm *alarm = container_of(timer, struct alarm, timer);

	CHECK(work == alarm->work);
	container_of(work, struct attached, work)->completed++;
	memset(alarm, 0xa5, sizeof(*alarm));
	free(alarm);
}

POOR_LOOP_GROUP_DONE(attached_work, loop, work)
{
	struct attached *a = container_of(work, struct attached, work);

	CHECK(!a->before.pending && !a->after.pending);
	a->calls++;
	poor_loop_stop(loop);
}

static int test_group_attach(void)
{
	struct attached a = {
		.before = POOR_LOOP_OP_INIT(attached_before_done),
		.work = POOR_LOOP_GROUP_INIT,
		.after = POOR_LOOP_OP_INIT(attached_after_done),
	};
	struct poor_loop loop;

	loop_init(&loop, 8);
	io_uring_prep_nop(poor_loop_group_get_sqe(&loop, &a.work, &a.before));
	io_uring_prep_nop(poor_loop_group_get_sqe(&loop, &a.work, &a.after));
	CHECK(!poor_loop_group_get_sqe(&loop, &a.work, &a.after));
	CHECK_EQ(errno, EBUSY);
	CHECK_EQ(a.work.group.pending, 2);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(a.completed, 2);
	CHECK_EQ(a.calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

static int test_group_attach_requests(void)
{
	struct attached a = { .work = POOR_LOOP_GROUP_INIT };
	struct poor_loop loop;

	loop_init(&loop, 8);
	for (unsigned i = 0; i < 2; i++) {
		struct request *req = malloc(sizeof(*req));

		CHECK(req);
		*req = (struct request){ .op = POOR_LOOP_OP_INIT(request_done), .work = &a.work };
		io_uring_prep_nop(poor_loop_group_get_sqe(&loop, req->work, &req->op));
	}
	CHECK_EQ(a.work.group.pending, 2);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(a.completed, 2);
	CHECK_EQ(a.calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

static int test_group_attach_timers(void)
{
	struct attached a = { .work = POOR_LOOP_GROUP_INIT };
	uint64_t later = poor_loop_now() + 10'000'000'000;
	struct alarm *alarms[2];
	struct poor_loop loop;

	loop_init(&loop, 8);
	for (unsigned i = 0; i < 2; i++) {
		alarms[i] = malloc(sizeof(*alarms[i]));
		CHECK(alarms[i]);
		poor_loop_timer_init(&alarms[i]->timer, alarm_fire);
		alarms[i]->work = &a.work;
		poor_loop_group_timer_arm(&loop, alarms[i]->work, &alarms[i]->timer, i ? later : poor_loop_now());
	}
	CHECK(!poor_loop_group_timer_disarm(alarms[1]->work, &alarms[1]->timer));
	free(alarms[1]);
	CHECK_EQ(a.work.group.pending, 1);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(a.completed, 1);
	CHECK_EQ(a.calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

static int test_group_header(void)
{
	struct poor_loop loop;
	struct shared s;

	shared_init(&s);
	loop_init(&loop, 8);
	poor_loop_group_acquire(&s.io);
	io_uring_prep_nop(poor_loop_group_get_sqe(&loop, &s.io, &s.nop));
	poor_loop_group_timer_arm(&loop, &s.io, &s.tick, poor_loop_now());
	CHECK_EQ(s.io.group.pending, 3);
	CHECK(!poor_loop_group_release(&s.io));
	CHECK_EQ(s.calls, 0);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(s.completed, 2);
	CHECK_EQ(s.calls, 1);
	poor_loop_group_acquire(&s.io);
	CHECK(poor_loop_group_release(&s.io));
	shared_finish(&loop, &s);
	CHECK_EQ(s.calls, 2);
	poor_loop_exit(&loop);
	return 0;
}

static const struct test tests[] = {
	TEST(group_empty),
	TEST(group_holds),
	TEST(group_release_pending),
	TEST(group_timer_disarm),
	TEST(group_sqe_failure),
	TEST(group_after_callback),
	TEST(group_reads_close),
	TEST(group_phases),
	TEST(group_multishot_free),
	TEST(group_timer),
	TEST(group_disarm_in_callback),
	TEST(group_layout),
	TEST(group_attach),
	TEST(group_attach_requests),
	TEST(group_attach_timers),
	TEST(group_header),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
