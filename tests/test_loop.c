/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <sys/time.h>

#include "harness.h"

static int test_init_exit(void)
{
	struct io_uring_params p;
	struct chio_loop loop;
	struct rec rec;
	int fd;

	setup_params(&p);
	memset(&loop, 0xa5, sizeof(loop));
	CHECK_EQ(chio_loop_init(&loop, 8, &p), 0);
	CHECK_EQ(loop.ring.sq.ring_entries, 8);
	CHECK(!loop.stop);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	fd = loop.ring.ring_fd;
	chio_loop_exit(&loop);
	CHECK_EQ(fcntl(fd, F_GETFD), -1);
	CHECK_EQ(errno, EBADF);
	return 0;
}

static int test_init_params(void)
{
	struct io_uring_params p;
	struct chio_loop loop;
	struct rec rec;

	setup_params(&p);
	p.flags |= IORING_SETUP_CQSIZE;
	p.cq_entries = 64;
	CHECK_EQ(chio_loop_init(&loop, 4, &p), 0);
	CHECK_EQ(p.sq_entries, 4);
	CHECK_EQ(p.cq_entries, 64);
	CHECK(p.features & IORING_FEAT_NODROP);
	CHECK_EQ(loop.ring.sq.ring_entries, 4);
	CHECK_EQ(loop.ring.cq.ring_entries, 64);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	chio_loop_exit(&loop);
	return 0;
}

static int test_init_errors(void)
{
	struct io_uring_params p;
	struct chio_loop loop;

	setup_params(&p);
	CHECK_EQ(chio_loop_init(&loop, 0, &p), -EINVAL);
	setup_params(&p);
	p.flags |= 1U << 31;
	CHECK_EQ(chio_loop_init(&loop, 8, &p), -EINVAL);
	return 0;
}

struct storm {
	struct rec rec;
	struct tick stopper;
};

static void storm_complete(struct chio_loop *loop, struct chio_op *op,
			   const struct io_uring_cqe *cqe)
{
	struct storm *storm = chio_container_of(op, struct storm, rec.op);

	rec_complete(loop, op, cqe);
	if (storm->rec.calls == 3)
		chio_timer_arm(loop, &storm->stopper.timer, 0);
	arm_nop(loop, op);
	CHECK(io_uring_submit(&loop->ring) > 0);
}

static int test_bounded_dispatch(void)
{
	struct chio_loop loop;
	struct storm storm;

	loop_init(&loop, 8);
	rec_init(&storm.rec, storm_complete);
	tick_init(&storm.stopper, stop_fire, 1);
	arm_nop(&loop, &storm.rec.op);
	CHECK_EQ(chio_loop_run(&loop), 0);
	CHECK_EQ(storm.stopper.fired, 1);
	CHECK(storm.rec.calls >= 3);
	CHECK(storm.rec.op.pending);
	chio_loop_exit(&loop);
	return 0;
}

static int submit_failures;
static int submit_error;

int io_uring_submit_and_wait(struct io_uring *ring, unsigned wait_nr)
{
	static int (*real)(struct io_uring *, unsigned);
	void *sym;

	if (submit_failures > 0) {
		submit_failures--;
		return submit_error;
	}
	if (!real) {
		sym = dlsym(RTLD_NEXT, "io_uring_submit_and_wait");
		CHECK(sym);
		memcpy(&real, &sym, sizeof(real));
	}
	return real(ring, wait_nr);
}

static int test_run_error(void)
{
	struct io_uring_params p;
	struct chio_loop loop;
	struct rec rec;

	setup_params(&p);
	p.flags |= IORING_SETUP_R_DISABLED;
	CHECK_EQ(chio_loop_init(&loop, 8, &p), 0);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	CHECK_EQ(chio_loop_run(&loop), -EBADFD);
	CHECK(rec.op.pending);
	enable_ring(&loop);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	chio_loop_exit(&loop);
	return 0;
}

static int test_submit_retry(void)
{
	static const int errors[] = { -EAGAIN, -ENOMEM };
	struct rec reader, nop;
	struct chio_loop loop;
	char buf[8];
	int fds[2];
	size_t i;

	for (i = 0; i < ARRAY_SIZE(errors); i++) {
		make_pipe(&fds);
		loop_init(&loop, 8);
		rec_init(&reader, rec_complete);
		rec_init(&nop, stop_complete);
		arm_read(&loop, &reader.op, fds[0], sizeof(buf), &buf);
		CHECK_EQ(io_uring_submit(&loop.ring), 1);
		CHECK_EQ(write(fds[1], "x", 1), 1);
		arm_nop(&loop, &nop.op);

		submit_error = errors[i];
		submit_failures = INT_MAX;
		drain(&loop, &reader.op);
		CHECK_EQ(reader.res, 1);
		CHECK_EQ(nop.calls, 0);

		submit_failures = 3;
		CHECK_EQ(chio_loop_run(&loop), 0);
		CHECK_EQ(submit_failures, 0);
		CHECK_EQ(nop.calls, 1);
		chio_loop_exit(&loop);
		close_pipe(&fds);
	}
	return 0;
}

static int test_stop(void)
{
	struct rec reader, stopper;
	struct chio_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&stopper, stop_complete);
	arm_read(&loop, &reader.op, fds[0], sizeof(buf), &buf);
	arm_nop(&loop, &stopper.op);
	CHECK_EQ(chio_loop_run(&loop), 0);
	CHECK_EQ(stopper.calls, 1);
	CHECK_EQ(reader.calls, 0);
	CHECK(reader.op.pending);
	CHECK(!loop.stop);

	chio_loop_stop(&loop);
	CHECK_EQ(chio_loop_run(&loop), 0);
	CHECK(!loop.stop);
	CHECK_EQ(reader.calls, 0);

	reader.op.complete = stop_complete;
	CHECK_EQ(write(fds[1], "x", 1), 1);
	CHECK_EQ(chio_loop_run(&loop), 0);
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, 1);
	CHECK(!reader.op.pending);
	chio_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static volatile sig_atomic_t alarms;
static int alarm_fd = -1;

static void on_alarm(int)
{
	ssize_t ret;

	alarms++;
	if (alarm_fd >= 0) {
		ret = write(alarm_fd, "x", 1);
		(void)ret;
	}
}

static void set_alarm(long usec)
{
	struct itimerval timer = {
		.it_interval.tv_usec = usec,
		.it_value.tv_usec = usec,
	};

	CHECK_EQ(setitimer(ITIMER_REAL, &timer, nullptr), 0);
}

static int test_eintr(void)
{
	struct sigaction sa = { .sa_handler = on_alarm }, old;
	struct chio_loop loop;
	char buf[1] = {};
	struct rec rec;
	int fds[2];

	make_pipe(&fds);
	alarms = 0;
	alarm_fd = fds[1];
	CHECK_EQ(sigemptyset(&sa.sa_mask), 0);
	CHECK_EQ(sigaction(SIGALRM, &sa, &old), 0);
	loop_init(&loop, 8);
	rec_init(&rec, stop_complete);
	arm_read(&loop, &rec.op, fds[0], sizeof(buf), &buf);
	CHECK_EQ(io_uring_submit(&loop.ring), 1);
	set_alarm(20000);
	CHECK_EQ(chio_loop_run(&loop), 0);
	set_alarm(0);
	CHECK(alarms >= 1);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, 1);
	CHECK_EQ(buf[0], 'x');
	CHECK(!rec.op.pending);

	chio_loop_exit(&loop);
	alarm_fd = -1;
	CHECK_EQ(sigaction(SIGALRM, &old, nullptr), 0);
	close_pipe(&fds);
	return 0;
}

static int test_exit_pending(void)
{
	struct chio_loop loop;
	struct rec rec;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_read(&loop, &rec.op, fds[0], sizeof(buf), &buf);
	CHECK_EQ(io_uring_submit(&loop.ring), 1);
	chio_loop_exit(&loop);
	CHECK_EQ(rec.calls, 0);
	CHECK(rec.op.pending);
	close_pipe(&fds);
	return 0;
}

const struct test tests[] = {
	TEST(init_exit),
	TEST(init_params),
	TEST(init_errors),
	TEST(bounded_dispatch),
	TEST(run_error),
	TEST(submit_retry),
	TEST(stop),
	TEST(eintr),
	TEST(exit_pending),
};

const size_t tests_count = ARRAY_SIZE(tests);
