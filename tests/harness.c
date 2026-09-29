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
	fprintf(stderr, "%s:%d: [%s] check failed: %s\n", file, line,
		mode->name, expr);
	exit(1);
}

[[noreturn]] void fail_eq(const char *file, int line, const char *a,
			  const char *b, long long va, long long vb)
{
	fprintf(stderr, "%s:%d: [%s] check failed: %s == %s (%lld != %lld)\n",
		file, line, mode->name, a, b, va, vb);
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
	CHECK_EQ(syscall(__NR_io_uring_register, poor_loop_ring(loop)->ring_fd,
			 IORING_REGISTER_ENABLE_RINGS, nullptr, 0), 0);
}

void make_pipe(int (*fds)[2])
{
	CHECK_EQ(pipe2(*fds, O_CLOEXEC), 0);
}

void close_pipe(int (*fds)[2])
{
	CHECK_EQ(close((*fds)[0]), 0);
	CHECK_EQ(close((*fds)[1]), 0);
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

void arm_read(struct poor_loop *loop, struct poor_loop_op *op, int fd,
	      unsigned len, char (*buf)[len])
{
	io_uring_prep_read(get_sqe(loop, op), fd, *buf, len, 0);
}

static struct poor_loop_op **awaited;
static size_t awaited_count;

static bool settled(void)
{
	for (size_t i = 0; i < awaited_count; i++)
		if (awaited[i]->pending)
			return false;
	return true;
}

void drain_ops(struct poor_loop *loop, size_t count,
	       struct poor_loop_op *(*ops)[count])
{
	awaited = *ops;
	awaited_count = count;
	if (!settled())
		CHECK_EQ(poor_loop_run(loop), 0);
	awaited_count = 0;
}

void rec_complete(struct poor_loop *loop, struct poor_loop_op *op,
		  const struct io_uring_cqe *cqe)
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

void stop_complete(struct poor_loop *loop, struct poor_loop_op *op,
		   const struct io_uring_cqe *cqe)
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
	printf("%s %s.%s\n", ret == SKIP ? "SKIP" : "PASS", mode->name,
	       test->name);
	return ret;
}

[[noreturn]] static void usage(void)
{
	size_t i;

	fprintf(stderr, "usage: test_loop MODE [TEST]\nmodes:");
	for (i = 0; i < ARRAY_SIZE(modes); i++)
		fprintf(stderr, " %s", modes[i].name);
	fprintf(stderr, "\ntests:");
	for (i = 0; i < tests_count; i++)
		fprintf(stderr, " %s", tests[i].name);
	fputc('\n', stderr);
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

int main(int argc, char **argv)
{
	const struct test *test = nullptr;
	size_t i;
	int ret;

	if (argc < 2 || argc > 3)
		usage();
	for (i = 0; i < ARRAY_SIZE(modes); i++)
		if (!strcmp(argv[1], modes[i].name))
			mode = &modes[i];
	if (!mode)
		usage();
	if (argc == 3) {
		for (i = 0; i < tests_count; i++)
			if (!strcmp(argv[2], tests[i].name))
				test = &tests[i];
		if (!test)
			usage();
	}

	ret = probe_mode();
	if (ret) {
		printf("SKIP %s: %s\n", mode->name, strerror(-ret));
		return SKIP;
	}
	if (test)
		return run_test(test);
	for (i = 0; i < tests_count; i++)
		run_test(&tests[i]);
	return 0;
}
