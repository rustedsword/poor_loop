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
	struct poor_loop loop;
	struct rec rec;
	int fd;

	setup_params(&p);
	memset(&loop, 0xa5, sizeof(loop));
	CHECK_EQ(poor_loop_init(&loop, 8, &p), 0);
	CHECK_EQ(poor_loop_ring(&loop)->sq.ring_entries, 8);
	CHECK(!poor_loop_stopped(&loop));
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	fd = poor_loop_ring(&loop)->ring_fd;
	poor_loop_exit(&loop);
	CHECK_EQ(fcntl(fd, F_GETFD), -1);
	CHECK_EQ(errno, EBADF);
	return 0;
}

static int test_init_params(void)
{
	struct io_uring_params p;
	struct poor_loop loop;
	struct rec rec;

	setup_params(&p);
	p.flags |= IORING_SETUP_CQSIZE;
	p.cq_entries = 64;
	CHECK_EQ(poor_loop_init(&loop, 4, &p), 0);
	CHECK_EQ(p.sq_entries, 4);
	CHECK_EQ(p.cq_entries, 64);
	CHECK(p.features & IORING_FEAT_NODROP);
	CHECK_EQ(poor_loop_ring(&loop)->sq.ring_entries, 4);
	CHECK_EQ(poor_loop_ring(&loop)->cq.ring_entries, 64);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

static int test_init_errors(void)
{
	struct io_uring_params p;
	struct poor_loop loop;

	setup_params(&p);
	CHECK_EQ(poor_loop_init(&loop, 0, &p), -EINVAL);
	setup_params(&p);
	p.flags |= 1U << 31;
	CHECK_EQ(poor_loop_init(&loop, 8, &p), -EINVAL);
	return 0;
}

struct storm {
	struct rec rec;
	struct tick stopper;
};

static void storm_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct storm *storm = container_of(op, struct storm, rec.op);

	rec_complete(loop, op, cqe);
	if (storm->rec.calls == 3)
		poor_loop_timer_arm(loop, &storm->stopper.timer, 0);
	arm_nop(loop, op);
	submit(loop, 1);
}

static int test_bounded_dispatch(void)
{
	struct poor_loop loop;
	struct storm storm;

	loop_init(&loop, 8);
	rec_init(&storm.rec, storm_complete);
	tick_init(&storm.stopper, stop_fire, 1);
	arm_nop(&loop, &storm.rec.op);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(storm.stopper.fired, 1);
	CHECK(storm.rec.calls >= 3);
	CHECK(storm.rec.op.pending);
	poor_loop_exit(&loop);
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
	struct poor_loop loop;
	struct rec rec;

	setup_params(&p);
	p.flags |= IORING_SETUP_R_DISABLED;
	CHECK_EQ(poor_loop_init(&loop, 8, &p), 0);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	CHECK_EQ(poor_loop_run(&loop), -EBADFD);
	CHECK_EQ(poor_loop_run_once(&loop), -EBADFD);
	CHECK(rec.op.pending);
	enable_ring(&loop);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	poor_loop_exit(&loop);
	return 0;
}

static int test_submit_retry(void)
{
	static const int errors[] = { -EAGAIN, -ENOMEM };
	struct rec reader, nop;
	struct poor_loop loop;
	char buf[8];
	int fds[2];

	foreach_array_ref(errors, error) {
		make_pipe(&fds);
		loop_init(&loop, 8);
		rec_init(&reader, rec_complete);
		rec_init(&nop, stop_complete);
		arm_read(&loop, &reader.op, fds[0], buf);
		submit(&loop, 1);
		CHECK_EQ(write(fds[1], "x", 1), 1);
		arm_nop(&loop, &nop.op);

		submit_error = *error;
		submit_failures = INT_MAX;
		drain(&loop, &reader.op);
		CHECK_EQ(reader.res, 1);
		CHECK_EQ(nop.calls, 0);

		submit_failures = 3;
		CHECK_EQ(poor_loop_run(&loop), 0);
		CHECK_EQ(submit_failures, 0);
		CHECK_EQ(nop.calls, 1);
		poor_loop_exit(&loop);
		close_pipe(&fds);
	}
	return 0;
}

static int test_stop(void)
{
	struct rec reader, stopper;
	struct poor_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&stopper, stop_complete);
	arm_read(&loop, &reader.op, fds[0], buf);
	arm_nop(&loop, &stopper.op);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(stopper.calls, 1);
	CHECK_EQ(reader.calls, 0);
	CHECK(reader.op.pending);
	CHECK(poor_loop_stopped(&loop));

	CHECK_EQ(write(fds[1], "x", 1), 1);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(reader.calls, 0);

	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, 1);
	CHECK(!reader.op.pending);
	CHECK(poor_loop_stopped(&loop));

	poor_loop_clear_stop(&loop);
	arm_nop(&loop, &stopper.op);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(stopper.calls, 2);
	CHECK(poor_loop_stopped(&loop));
	poor_loop_exit(&loop);
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
	struct poor_loop loop;
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
	arm_read(&loop, &rec.op, fds[0], buf);
	submit(&loop, 1);
	set_alarm(20000);
	CHECK_EQ(poor_loop_run(&loop), 0);
	set_alarm(0);
	CHECK(alarms >= 1);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, 1);
	CHECK_EQ(buf[0], 'x');
	CHECK(!rec.op.pending);

	poor_loop_exit(&loop);
	alarm_fd = -1;
	CHECK_EQ(sigaction(SIGALRM, &old, nullptr), 0);
	close_pipe(&fds);
	return 0;
}

static int test_run_once(void)
{
	struct rec reader, nop;
	struct poor_loop loop;
	struct tick tick;
	uint64_t deadline;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&nop, rec_complete);
	arm_read(&loop, &reader.op, fds[0], buf);
	arm_nop(&loop, &nop.op);
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(nop.calls, 1);
	CHECK_EQ(reader.calls, 0);
	CHECK(reader.op.pending);

	tick_init(&tick, tick_fire, 1);
	deadline = poor_loop_now() + 2'000'000;
	poor_loop_timer_arm(&loop, &tick.timer, deadline);
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(tick.fired, 1);
	CHECK(tick.at >= deadline);
	CHECK_EQ(reader.calls, 0);

	CHECK_EQ(write(fds[1], "x", 1), 1);
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, 1);
	CHECK(!reader.op.pending);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_run_once_eintr(void)
{
	struct sigaction sa = { .sa_handler = on_alarm }, old;
	struct poor_loop loop;
	char buf[1];
	struct rec rec;
	int fds[2];

	make_pipe(&fds);
	alarms = 0;
	CHECK_EQ(sigemptyset(&sa.sa_mask), 0);
	CHECK_EQ(sigaction(SIGALRM, &sa, &old), 0);
	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_read(&loop, &rec.op, fds[0], buf);
	set_alarm(20000);
	CHECK_EQ(poor_loop_run_once(&loop), 0);
	set_alarm(0);
	CHECK(alarms >= 1);
	CHECK_EQ(rec.calls, 0);
	CHECK(rec.op.pending);

	CHECK_EQ(write(fds[1], "x", 1), 1);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.res, 1);
	poor_loop_exit(&loop);
	CHECK_EQ(sigaction(SIGALRM, &old, nullptr), 0);
	close_pipe(&fds);
	return 0;
}

static int test_exit_pending(void)
{
	struct poor_loop loop;
	struct rec rec;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_read(&loop, &rec.op, fds[0], buf);
	submit(&loop, 1);
	poor_loop_exit(&loop);
	CHECK_EQ(rec.calls, 0);
	CHECK(rec.op.pending);
	close_pipe(&fds);
	return 0;
}

static uint64_t later(void)
{
	return poor_loop_now() + 10'000'000'000;
}

static int test_drain_idle(void)
{
	struct poor_loop loop;

	loop_init(&loop, 8);
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);
	CHECK(!poor_loop_stopped(&loop));
	CHECK_EQ(io_uring_cq_ready(poor_loop_ring(&loop)), 0);
	poor_loop_exit(&loop);
	return 0;
}

static const char x[] = { 'x' };

static struct io_uring_sqe *get_untracked_sqe(struct poor_loop *loop)
{
	auto sqe = poor_loop_get_untracked_sqe(loop);

	CHECK(sqe);
	return sqe;
}

static int test_drain(void)
{
	struct __kernel_timespec ts = { .tv_nsec = 2'000'000 };
	struct poor_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	CHECK_EQ(fcntl(fds[0], F_SETFL, O_NONBLOCK), 0);
	loop_init(&loop, 8);
	auto sqe = get_untracked_sqe(&loop);
	io_uring_prep_timeout(sqe, &ts, 0, 0);
	sqe->flags |= IOSQE_IO_HARDLINK;
	io_uring_prep_write_array(get_untracked_sqe(&loop), fds[1], x, 0);
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);
	CHECK_EQ(read(fds[0], buf, sizeof(buf)), 1);
	CHECK_EQ(buf[0], 'x');
	CHECK_EQ(io_uring_cq_ready(poor_loop_ring(&loop)), 0);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

struct closer {
	struct rec reader, cancel;
	int fd;
};

static void closer_read(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	auto sqe = get_untracked_sqe(loop);

	rec_complete(loop, op, cqe);
	io_uring_prep_close(sqe, container_of(op, struct closer, reader.op)->fd);
	sqe->flags |= IOSQE_ASYNC;
}

static int test_drain_close(void)
{
	struct closer closer;
	struct poor_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&closer.reader, closer_read);
	rec_init(&closer.cancel, rec_complete);
	closer.fd = fcntl(fds[0], F_DUPFD_CLOEXEC, 0);
	CHECK(closer.fd >= 0);
	arm_read(&loop, &closer.reader.op, closer.fd, buf);
	submit(&loop, 1);
	io_uring_prep_cancel(get_sqe(&loop, &closer.cancel.op), &closer.reader.op, 0);
	drain(&loop, &closer.reader.op, &closer.cancel.op);
	CHECK_EQ(closer.reader.res, -ECANCELED);
	CHECK_EQ(closer.cancel.res, 0);
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);
	CHECK_EQ(fcntl(closer.fd, F_GETFD), -1);
	CHECK_EQ(errno, EBADF);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_drain_op(void)
{
	struct poor_loop loop;
	struct rec rec;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	io_uring_prep_read_array(get_untracked_sqe(&loop), fds[0], buf, 0);
	arm_nop(&loop, &rec.op);
	CHECK_EQ(poor_loop_drain(&loop, later()), -EBUSY);
	CHECK_EQ(rec.calls, 0);
	CHECK(rec.op.pending);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_drain_timer(void)
{
	struct poor_loop loop;
	struct tick tick;

	loop_init(&loop, 8);
	tick_init(&tick, tick_fire, 1);
	poor_loop_timer_arm(&loop, &tick.timer, later());
	CHECK_EQ(poor_loop_drain(&loop, later()), -EBUSY);
	CHECK(poor_loop_timer_armed(&tick.timer));
	CHECK_EQ(io_uring_sq_ready(poor_loop_ring(&loop)), 0);
	poor_loop_timer_disarm(&tick.timer);
	poor_loop_exit(&loop);
	return 0;
}

static int test_drain_link(void)
{
	struct poor_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	io_uring_prep_read_array(get_untracked_sqe(&loop), fds[0], buf, 0);
	submit(&loop, 1);
	auto sqe = get_untracked_sqe(&loop);
	io_uring_prep_write_array(sqe, fds[1], x, 0);
	sqe->flags |= IOSQE_IO_LINK;
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);

	sqe = get_untracked_sqe(&loop);
	io_uring_prep_write_array(sqe, -1, x, 0);
	sqe->flags |= IOSQE_IO_LINK;
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);
	CHECK_EQ(io_uring_cq_ready(poor_loop_ring(&loop)), 0);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_drain_sq_full(void)
{
	struct poor_loop loop;

	loop_init(&loop, 1);
	io_uring_prep_nop(get_untracked_sqe(&loop));
	CHECK_EQ(io_uring_sq_space_left(poor_loop_ring(&loop)), 0);
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);
	CHECK_EQ(io_uring_cq_ready(poor_loop_ring(&loop)), 0);
	poor_loop_exit(&loop);
	return 0;
}

static int test_drain_skip_success(void)
{
	struct io_uring_params p;
	struct poor_loop loop;
	int ret = 0;

	setup_params(&p);
	CHECK_EQ(poor_loop_init(&loop, 8, &p), 0);
	if (p.features & IORING_FEAT_CQE_SKIP) {
		auto sqe = get_untracked_sqe(&loop);
		io_uring_prep_nop(sqe);
		sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
		CHECK_EQ(poor_loop_drain(&loop, later()), -EOPNOTSUPP);
	} else {
		ret = SKIP;
	}
	poor_loop_exit(&loop);
	return ret;
}

static int test_drain_no_ext_arg(void)
{
	struct poor_loop loop;

	loop_init(&loop, 8);
	poor_loop_ring(&loop)->features &= ~IORING_FEAT_EXT_ARG;
	CHECK_EQ(poor_loop_drain(&loop, later()), -EOPNOTSUPP);
	CHECK_EQ(io_uring_sq_ready(poor_loop_ring(&loop)), 0);
	poor_loop_exit(&loop);
	return 0;
}

static int test_drain_error(void)
{
	struct io_uring_params p;
	struct poor_loop loop;

	setup_params(&p);
	p.flags |= IORING_SETUP_R_DISABLED;
	CHECK_EQ(poor_loop_init(&loop, 8, &p), 0);
	CHECK_EQ(poor_loop_drain(&loop, later()), -EBADFD);
	poor_loop_exit(&loop);
	return 0;
}

static int test_drain_eintr(void)
{
	struct sigaction sa = { .sa_handler = on_alarm }, old;
	struct poor_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	alarms = 0;
	CHECK_EQ(sigemptyset(&sa.sa_mask), 0);
	CHECK_EQ(sigaction(SIGALRM, &sa, &old), 0);
	loop_init(&loop, 8);
	io_uring_prep_read_array(get_untracked_sqe(&loop), fds[0], buf, 0);
	set_alarm(20000);
	CHECK_EQ(poor_loop_drain(&loop, later()), -EINTR);
	set_alarm(0);
	CHECK(alarms >= 1);

	CHECK_EQ(write(fds[1], "x", 1), 1);
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);
	poor_loop_exit(&loop);
	CHECK_EQ(sigaction(SIGALRM, &old, nullptr), 0);
	close_pipe(&fds);
	return 0;
}

static int test_drain_past_deadline(void)
{
	struct poor_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	io_uring_prep_read_array(get_untracked_sqe(&loop), fds[0], buf, 0);
	CHECK_EQ(poor_loop_drain(&loop, 0), -ETIME);
	if (!(mode->flags & IORING_SETUP_SQPOLL))
		CHECK_EQ(io_uring_sq_ready(poor_loop_ring(&loop)), 0);

	CHECK_EQ(write(fds[1], "x", 1), 1);
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_drain_deadline(void)
{
	struct poor_loop loop;
	uint64_t deadline;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	io_uring_prep_read_array(get_untracked_sqe(&loop), fds[0], buf, 0);
	deadline = poor_loop_now() + 5'000'000;
	CHECK_EQ(poor_loop_drain(&loop, deadline), -ETIME);
	CHECK(poor_loop_now() >= deadline);

	CHECK_EQ(write(fds[1], "x", 1), 1);
	CHECK_EQ(poor_loop_drain(&loop, later()), 0);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static const struct test tests[] = {
	TEST(init_exit),
	TEST(init_params),
	TEST(init_errors),
	TEST(bounded_dispatch),
	TEST(run_error),
	TEST(submit_retry),
	TEST(stop),
	TEST(eintr),
	TEST(run_once),
	TEST(run_once_eintr),
	TEST(exit_pending),
	TEST(drain_idle),
	TEST(drain),
	TEST(drain_close),
	TEST(drain_op),
	TEST(drain_timer),
	TEST(drain_link),
	TEST(drain_sq_full),
	TEST(drain_skip_success),
	TEST(drain_no_ext_arg),
	TEST(drain_error),
	TEST(drain_eintr),
	TEST(drain_past_deadline),
	TEST(drain_deadline),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
