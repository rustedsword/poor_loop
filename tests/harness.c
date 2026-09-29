/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <dirent.h>
#include <sys/syscall.h>

#include "harness.h"

static const struct mode modes[] = {
	{ "default", 0 },
	{ "coop", IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG },
	{ "defer", IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN },
	{ "sqpoll", IORING_SETUP_SQPOLL },
};

const struct mode *mode;
uint64_t tick_order;

[[noreturn]] void fail(const char *file, int line, const char *expr)
{
	printerrln(file, ":", line, ": [", mode->name, "] check failed: ", expr);
	exit(1);
}

[[noreturn]] void fail_eq(const char *file, int line, const char *a, const char *b, long long va, long long vb)
{
	printerrln(file, ":", line, ": [", mode->name, "] check failed: ", a, " == ", b, " (", va, " != ", vb, ")");
	exit(1);
}

void setup_params(struct io_uring_params *p)
{
	memset(p, 0, sizeof(*p));
	p->flags = mode->flags;
	if (p->flags & IORING_SETUP_SQPOLL)
		p->sq_thread_idle = 10;
}

void loop_init(struct poor_loop *loop, unsigned entries)
{
	struct io_uring_params p;

	setup_params(&p);
	CHECK_EQ(poor_loop_init(loop, entries, &p), 0);
}

void enable_ring(struct poor_loop *loop)
{
	CHECK_EQ(syscall(__NR_io_uring_register, poor_loop_ring(loop)->ring_fd, IORING_REGISTER_ENABLE_RINGS, nullptr, 0), 0);
}

void make_pipe(int (*fds)[2])
{
	CHECK_EQ(pipe2(*fds, O_CLOEXEC), 0);
}

void close_pipe(int (*fds)[2])
{
	foreach_array_ref(fds, fd)
		CHECK_EQ(close(*fd), 0);
}

struct io_uring_sqe *get_sqe(struct poor_loop *loop, struct poor_loop_op *op)
{
	struct io_uring_sqe *sqe = poor_loop_get_sqe_or_submit(loop, op);

	CHECK_EQ(sqe ? 0 : errno, 0);
	CHECK(op->pending);
	return sqe;
}

void arm_nop(struct poor_loop *loop, struct poor_loop_op *op)
{
	io_uring_prep_nop(get_sqe(loop, op));
}

void _arm_read(struct poor_loop *loop, struct poor_loop_op *op, int fd, size_t len, char (*buf)[len])
{
	io_uring_prep_read(get_sqe(loop, op), fd, *buf, len, 0);
}

static size_t awaited_count;
static struct poor_loop_op *(*awaited)[];

static bool settled(void)
{
	struct poor_loop_op *(*ops)[awaited_count] = awaited;

	foreach_array_ref(ops, op)
		if ((*op)->pending)
			return false;
	return true;
}

void drain_ops(struct poor_loop *loop, size_t count, struct poor_loop_op *(*ops)[count])
{
	awaited = ops;
	awaited_count = count;
	if (!settled())
		CHECK_EQ(poor_loop_run(loop), 0);
	awaited_count = 0;
}

void rec_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct rec *rec = container_of(op, struct rec, op);

	CHECK_EQ(op->pending, !!(cqe->flags & IORING_CQE_F_MORE));
	CHECK(io_uring_cqe_get_data(cqe) == op);
	rec->calls++;
	if (cqe->flags & IORING_CQE_F_MORE)
		rec->more++;
	rec->res = cqe->res;
	rec->flags = cqe->flags;
	if (awaited_count && settled())
		poor_loop_stop(loop);
}

void rec_init(struct rec *rec, poor_loop_complete_fn *complete)
{
	*rec = (struct rec){ .op = POOR_LOOP_OP_INIT(complete) };
}

void stop_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	rec_complete(loop, op, cqe);
	poor_loop_stop(loop);
}

void tick_fire(struct poor_loop *, struct poor_loop_timer *timer)
{
	struct tick *tick = container_of(timer, struct tick, timer);

	tick->fired++;
	tick->armed = poor_loop_timer_armed(timer);
	tick->at = poor_loop_now();
	tick_order = tick_order * 10 + tick->id;
}

void tick_init(struct tick *tick, poor_loop_timer_fn *fire, int id)
{
	*tick = (struct tick){ .timer = POOR_LOOP_TIMER_INIT(fire), .id = id };
}

void stop_fire(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	tick_fire(loop, timer);
	poor_loop_stop(loop);
}

void halt(struct poor_loop *loop, struct poor_loop_timer *)
{
	poor_loop_stop(loop);
}

static int count_fds(void)
{
	struct dirent *entry;
	int count = 0;
	DIR *dir;

	dir = opendir("/proc/self/fd");
	CHECK(dir);
	while ((entry = readdir(dir)))
		count++;
	CHECK_EQ(closedir(dir), 0);
	return count;
}

static int run_test(const struct test *test)
{
	int fds = count_fds();
	int ret = test->fn();

	CHECK_EQ(count_fds(), fds);
	println(ret == SKIP ? "SKIP" : "PASS", " ", mode->name, ".", test->name);
	return ret;
}

[[noreturn]] static void usage(size_t count, const struct test (*tests)[count])
{
	printerr("usage: test_loop MODE [TEST]\nmodes:");
	foreach_array_ref(modes, m)
		printerr(" ", m->name);
	printerr("\ntests:");
	foreach_array_ref(tests, test)
		printerr(" ", test->name);
	printerr((char)'\n');
	exit(2);
}

static int probe_mode(void)
{
	struct io_uring_params p;
	struct io_uring ring;
	int ret;

	setup_params(&p);
	ret = io_uring_queue_init_params(4, &ring, &p);
	if (!ret)
		io_uring_queue_exit(&ring);
	return ret;
}

int _run_tests(size_t argc, char *(*argv)[argc], size_t count, const struct test (*tests)[count])
{
	const struct test *test = nullptr;
	int ret;

	if (argc < 2 || argc > 3)
		usage(count, tests);
	foreach_array_ref(modes, m)
		if (!strcmp(arr(argv)[1], m->name))
			mode = m;
	if (!mode)
		usage(count, tests);
	if (argc == 3) {
		foreach_array_ref(tests, t)
			if (!strcmp(arr(argv)[2], t->name))
				test = t;
		if (!test)
			usage(count, tests);
	}

	ret = probe_mode();
	if (ret) {
		println("SKIP ", mode->name, ": ", strerror(-ret));
		return SKIP;
	}
	if (test)
		return run_test(test);
	foreach_array_ref(tests, t)
		run_test(t);
	return 0;
}
