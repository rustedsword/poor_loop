/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <chioloop.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef IORING_TIMEOUT_MULTISHOT
#define IORING_TIMEOUT_MULTISHOT (1U << 6)
#endif

#define SKIP 77
#define MANY 1000
#define RESUBMITS 100
#define CHAIN 10
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define container_of(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#define CHECK(cond) \
	do { \
		if (!(cond)) \
			fail(__FILE__, __LINE__, #cond); \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		long long a_ = (long long)(a), b_ = (long long)(b); \
		if (a_ != b_) \
			fail_eq(__FILE__, __LINE__, #a, #b, a_, b_); \
	} while (0)

static_assert(sizeof(struct chio_op) == sizeof(void *) + 8);
static_assert(offsetof(struct chio_op, pending) == sizeof(void *));
static_assert(offsetof(struct chio_op, data) == sizeof(void *) + 1);
static_assert(sizeof(((struct chio_op *)nullptr)->data) == 7);

struct mode {
	const char *name;
	unsigned flags;
};

static const struct mode modes[] = {
	{ "default", 0 },
	{ "coop", IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG },
	{ "defer", IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN },
	{ "sqpoll", IORING_SETUP_SQPOLL },
};

static const struct mode *mode;

[[noreturn]] static void fail(const char *file, int line, const char *expr)
{
	fprintf(stderr, "%s:%d: [%s] check failed: %s\n", file, line,
		mode->name, expr);
	exit(1);
}

[[noreturn]] static void fail_eq(const char *file, int line, const char *a,
				  const char *b, long long va, long long vb)
{
	fprintf(stderr, "%s:%d: [%s] check failed: %s == %s (%lld != %lld)\n",
		file, line, mode->name, a, b, va, vb);
	exit(1);
}

static void setup_params(struct io_uring_params *p)
{
	memset(p, 0, sizeof(*p));
	p->flags = mode->flags;
	if (p->flags & IORING_SETUP_SQPOLL)
		p->sq_thread_idle = 10;
}

static void loop_init(struct chio_loop *loop, unsigned entries)
{
	struct io_uring_params p;

	setup_params(&p);
	CHECK_EQ(chio_loop_init(loop, entries, &p), 0);
}

static void enable_ring(struct chio_loop *loop)
{
	CHECK_EQ(syscall(__NR_io_uring_register, loop->ring.ring_fd,
			 IORING_REGISTER_ENABLE_RINGS, nullptr, 0), 0);
}

static void make_pipe(int fds[2])
{
	CHECK_EQ(pipe2(fds, O_CLOEXEC), 0);
}

static void close_pipe(int fds[2])
{
	CHECK_EQ(close(fds[0]), 0);
	CHECK_EQ(close(fds[1]), 0);
}

static struct io_uring_sqe *get_sqe(struct chio_loop *loop,
				    struct chio_op *op)
{
	struct io_uring_sqe *sqe = chio_get_sqe(loop, op);

	CHECK_EQ(sqe ? 0 : errno, 0);
	CHECK(op->pending);
	return sqe;
}

static void arm_nop(struct chio_loop *loop, struct chio_op *op)
{
	io_uring_prep_nop(get_sqe(loop, op));
}

static void arm_read(struct chio_loop *loop, struct chio_op *op, int fd,
		     void *buf, unsigned len)
{
	io_uring_prep_read(get_sqe(loop, op), fd, buf, len, 0);
}

static void drain(struct chio_loop *loop, struct chio_op *op)
{
	while (op->pending)
		CHECK(chio_loop_run_once(loop, true) >= 0);
}

struct rec {
	struct chio_op op;
	int calls;
	int more;
	int res;
	unsigned flags;
};

static void rec_complete(struct chio_loop *, struct chio_op *op,
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
}

static void rec_init(struct rec *rec, chio_complete_fn *complete)
{
	*rec = (struct rec){ .op = CHIO_OP_INIT(complete) };
}

static void stop_complete(struct chio_loop *loop, struct chio_op *op,
			  const struct io_uring_cqe *cqe)
{
	rec_complete(loop, op, cqe);
	chio_loop_stop(loop);
}

static int test_op_init(void)
{
	struct chio_op lit = CHIO_OP_INIT(rec_complete), op;
	static const char zero[7];

	CHECK(lit.complete == rec_complete);
	CHECK(!lit.pending);
	CHECK(!memcmp(lit.data, zero, sizeof(zero)));
	memset(&op, 0xa5, sizeof(op));
	chio_op_init(&op, rec_complete);
	CHECK(op.complete == rec_complete);
	CHECK(!op.pending);
	CHECK(!memcmp(op.data, zero, sizeof(zero)));
	return 0;
}

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

static int test_nop(void)
{
	static const char pattern[7] = { 1, -2, 3, -4, 5, -6, 7 };
	struct chio_loop loop;
	struct rec rec;

	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	memcpy(rec.op.data, pattern, sizeof(pattern));
	arm_nop(&loop, &rec.op);
	CHECK_EQ(rec.calls, 0);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, 0);
	CHECK(!(rec.flags & IORING_CQE_F_MORE));
	CHECK(rec.op.complete == rec_complete);
	CHECK(!memcmp(rec.op.data, pattern, sizeof(pattern)));
	chio_loop_exit(&loop);
	return 0;
}

static int test_nowait(void)
{
	struct chio_loop loop;
	char buf[8] = {};
	struct rec rec;
	int fds[2];

	make_pipe(fds);
	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_read(&loop, &rec.op, fds[0], buf, sizeof(buf));
	CHECK_EQ(chio_loop_run_once(&loop, false), 0);
	CHECK_EQ(chio_loop_run_once(&loop, false), 0);
	CHECK_EQ(rec.calls, 0);
	CHECK(rec.op.pending);
	CHECK_EQ(write(fds[1], "ping", 4), 4);
	CHECK_EQ(chio_loop_run_once(&loop, true), 1);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, 4);
	CHECK(!memcmp(buf, "ping", 4));
	CHECK(!rec.op.pending);
	chio_loop_exit(&loop);
	close_pipe(fds);
	return 0;
}

static int test_batch(void)
{
	struct chio_loop loop;
	struct rec recs[4];
	size_t i;

	loop_init(&loop, 8);
	for (i = 0; i < ARRAY_SIZE(recs); i++) {
		rec_init(&recs[i], rec_complete);
		arm_nop(&loop, &recs[i].op);
	}
	CHECK_EQ(chio_loop_run_once(&loop, true), ARRAY_SIZE(recs));
	for (i = 0; i < ARRAY_SIZE(recs); i++) {
		CHECK_EQ(recs[i].calls, 1);
		CHECK_EQ(recs[i].res, 0);
		CHECK(!recs[i].op.pending);
	}
	chio_loop_exit(&loop);
	return 0;
}

static void chain_complete(struct chio_loop *loop, struct chio_op *op,
			   const struct io_uring_cqe *cqe)
{
	struct rec *rec = container_of(op, struct rec, op);

	rec_complete(loop, op, cqe);
	if (rec->calls < CHAIN) {
		arm_nop(loop, op);
		CHECK(io_uring_submit(&loop->ring) > 0);
	}
}

static int test_bounded_dispatch(void)
{
	struct chio_loop loop;
	struct rec rec;
	int i;

	loop_init(&loop, 8);
	rec_init(&rec, chain_complete);
	arm_nop(&loop, &rec.op);
	for (i = 1; i <= CHAIN; i++) {
		CHECK_EQ(chio_loop_run_once(&loop, true), 1);
		CHECK_EQ(rec.calls, i);
		if (i < CHAIN && !(mode->flags & IORING_SETUP_SQPOLL))
			CHECK_EQ(io_uring_cq_ready(&loop.ring), 1);
	}
	CHECK(!rec.op.pending);
	CHECK_EQ(chio_loop_run_once(&loop, false), 0);
	chio_loop_exit(&loop);
	return 0;
}

static void resubmit_complete(struct chio_loop *loop, struct chio_op *op,
			      const struct io_uring_cqe *cqe)
{
	uint32_t count;

	CHECK_EQ(cqe->res, 0);
	CHECK(!op->pending);
	memcpy(&count, op->data, sizeof(count));
	count++;
	memcpy(op->data, &count, sizeof(count));
	if (count < RESUBMITS)
		arm_nop(loop, op);
}

static int test_resubmit(void)
{
	struct chio_op op = CHIO_OP_INIT(resubmit_complete);
	struct chio_loop loop;
	uint32_t count;

	loop_init(&loop, 4);
	arm_nop(&loop, &op);
	drain(&loop, &op);
	memcpy(&count, op.data, sizeof(count));
	CHECK_EQ(count, RESUBMITS);
	chio_loop_exit(&loop);
	return 0;
}

static int test_sq_full(void)
{
	struct chio_loop loop;
	struct rec *recs;
	unsigned i;

	recs = calloc(MANY, sizeof(*recs));
	CHECK(recs);
	loop_init(&loop, 4);
	for (i = 0; i < MANY; i++) {
		rec_init(&recs[i], rec_complete);
		arm_nop(&loop, &recs[i].op);
	}
	for (i = 0; i < MANY; i++)
		drain(&loop, &recs[i].op);
	for (i = 0; i < MANY; i++) {
		CHECK_EQ(recs[i].calls, 1);
		CHECK_EQ(recs[i].res, 0);
	}
	chio_loop_exit(&loop);
	free(recs);
	return 0;
}

static int test_submit_error(void)
{
	struct io_uring_params p;
	struct chio_loop loop;
	struct rec recs[3];
	size_t i;

	setup_params(&p);
	p.flags |= IORING_SETUP_R_DISABLED;
	CHECK_EQ(chio_loop_init(&loop, 2, &p), 0);
	for (i = 0; i < ARRAY_SIZE(recs); i++)
		rec_init(&recs[i], rec_complete);
	arm_nop(&loop, &recs[0].op);
	arm_nop(&loop, &recs[1].op);
	errno = 0;
	CHECK(!chio_get_sqe(&loop, &recs[2].op));
	CHECK_EQ(errno, EBADFD);
	CHECK(!recs[2].op.pending);
	enable_ring(&loop);
	drain(&loop, &recs[0].op);
	drain(&loop, &recs[1].op);
	CHECK_EQ(recs[0].calls, 1);
	CHECK_EQ(recs[1].calls, 1);
	CHECK_EQ(recs[2].calls, 0);
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
	CHECK_EQ(chio_loop_run_once(&loop, false), -EBADFD);
	CHECK_EQ(chio_loop_run_once(&loop, true), -EBADFD);
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
		make_pipe(fds);
		loop_init(&loop, 8);
		rec_init(&reader, rec_complete);
		rec_init(&nop, stop_complete);
		arm_read(&loop, &reader.op, fds[0], buf, sizeof(buf));
		CHECK_EQ(chio_loop_run_once(&loop, false), 0);
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
		close_pipe(fds);
	}
	return 0;
}

static int test_double_arm(void)
{
	struct chio_loop loop;
	struct rec rec;

	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	errno = 0;
	CHECK(!chio_get_sqe(&loop, &rec.op));
	CHECK_EQ(errno, EBUSY);
	CHECK(rec.op.pending);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	arm_nop(&loop, &rec.op);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 2);
	chio_loop_exit(&loop);
	return 0;
}

static int test_untracked(void)
{
	struct io_uring_sqe *sqe;
	struct chio_loop loop;
	struct rec rec;

	loop_init(&loop, 8);
	sqe = io_uring_get_sqe(&loop.ring);
	CHECK(sqe);
	io_uring_prep_nop(sqe);
	io_uring_sqe_set_data(sqe, nullptr);
	rec_init(&rec, rec_complete);
	arm_nop(&loop, &rec.op);
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(io_uring_cq_ready(&loop.ring), 0);
	chio_loop_exit(&loop);
	return 0;
}

static int test_op_error(void)
{
	struct chio_loop loop;
	struct rec rec;
	char buf[8];

	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_read(&loop, &rec.op, -1, buf, sizeof(buf));
	drain(&loop, &rec.op);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, -EBADF);
	chio_loop_exit(&loop);
	return 0;
}

static int test_pipe_io(void)
{
	static const char msg[] = "hello, io_uring";
	struct rec reader, writer;
	struct chio_loop loop;
	char buf[64] = {};
	int fds[2];

	make_pipe(fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&writer, rec_complete);
	arm_read(&loop, &reader.op, fds[0], buf, sizeof(buf));
	io_uring_prep_write(get_sqe(&loop, &writer.op), fds[1], msg,
			    sizeof(msg), 0);
	drain(&loop, &reader.op);
	drain(&loop, &writer.op);
	CHECK_EQ(writer.calls, 1);
	CHECK_EQ(writer.res, sizeof(msg));
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, sizeof(msg));
	CHECK(!memcmp(buf, msg, sizeof(msg)));
	chio_loop_exit(&loop);
	close_pipe(fds);
	return 0;
}

static int test_linked_timeout(void)
{
	struct __kernel_timespec ts = { .tv_nsec = 1000000 };
	struct io_uring_sqe *sqe;
	struct rec reader, timer;
	struct chio_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&timer, rec_complete);
	sqe = get_sqe(&loop, &reader.op);
	io_uring_prep_read(sqe, fds[0], buf, sizeof(buf), 0);
	sqe->flags |= IOSQE_IO_LINK;
	io_uring_prep_link_timeout(get_sqe(&loop, &timer.op), &ts, 0);
	drain(&loop, &reader.op);
	drain(&loop, &timer.op);
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, -ECANCELED);
	CHECK_EQ(timer.calls, 1);
	CHECK_EQ(timer.res, -ETIME);
	chio_loop_exit(&loop);
	close_pipe(fds);
	return 0;
}

static void shot_complete(struct chio_loop *loop, struct chio_op *op,
			  const struct io_uring_cqe *cqe)
{
	rec_complete(loop, op, cqe);
	if (cqe->flags & IORING_CQE_F_MORE)
		CHECK_EQ(cqe->res, -ETIME);
}

static int test_multishot(void)
{
	struct __kernel_timespec ts = { .tv_nsec = 1000000 };
	struct chio_loop loop;
	struct rec rec;
	int ret = 0;

	loop_init(&loop, 8);
	rec_init(&rec, shot_complete);
	io_uring_prep_timeout(get_sqe(&loop, &rec.op), &ts, 3,
			      IORING_TIMEOUT_MULTISHOT);
	drain(&loop, &rec.op);
	if (rec.calls == 1 && rec.res == -EINVAL) {
		ret = SKIP;
	} else {
		CHECK_EQ(rec.calls, 3);
		CHECK_EQ(rec.more, 2);
		CHECK_EQ(rec.res, -ETIME);
		CHECK(!(rec.flags & IORING_CQE_F_MORE));
	}
	chio_loop_exit(&loop);
	return ret;
}

struct poll_cancel {
	struct rec poll;
	struct rec cancel;
};

static void poll_complete(struct chio_loop *loop, struct chio_op *op,
			  const struct io_uring_cqe *cqe)
{
	struct poll_cancel *pc = container_of(op, struct poll_cancel, poll.op);

	rec_complete(loop, op, cqe);
	if (!(cqe->flags & IORING_CQE_F_MORE) || pc->poll.more != 1)
		return;
	CHECK(cqe->res & POLLIN);
	io_uring_prep_cancel(get_sqe(loop, &pc->cancel.op), op, 0);
}

static int test_multishot_cancel(void)
{
	struct poll_cancel pc;
	struct chio_loop loop;
	int fds[2];

	make_pipe(fds);
	loop_init(&loop, 8);
	rec_init(&pc.poll, poll_complete);
	rec_init(&pc.cancel, rec_complete);
	io_uring_prep_poll_multishot(get_sqe(&loop, &pc.poll.op), fds[0],
				     POLLIN);
	CHECK_EQ(chio_loop_run_once(&loop, false), 0);
	CHECK_EQ(write(fds[1], "x", 1), 1);
	drain(&loop, &pc.poll.op);
	drain(&loop, &pc.cancel.op);
	CHECK(pc.poll.more >= 1);
	CHECK_EQ(pc.poll.calls, pc.poll.more + 1);
	CHECK_EQ(pc.poll.res, -ECANCELED);
	CHECK(!(pc.poll.flags & IORING_CQE_F_MORE));
	CHECK_EQ(pc.cancel.calls, 1);
	CHECK_EQ(pc.cancel.res, 0);
	chio_loop_exit(&loop);
	close_pipe(fds);
	return 0;
}

static void cancel_read(bool submitted)
{
	struct rec reader, cancel;
	struct chio_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&cancel, rec_complete);
	arm_read(&loop, &reader.op, fds[0], buf, sizeof(buf));
	if (submitted)
		CHECK_EQ(chio_loop_run_once(&loop, false), 0);
	io_uring_prep_cancel(get_sqe(&loop, &cancel.op), &reader.op, 0);
	drain(&loop, &reader.op);
	drain(&loop, &cancel.op);
	CHECK_EQ(reader.calls, 1);
	CHECK_EQ(reader.res, -ECANCELED);
	CHECK_EQ(cancel.calls, 1);
	CHECK_EQ(cancel.res, 0);
	chio_loop_exit(&loop);
	close_pipe(fds);
}

static int test_cancel(void)
{
	cancel_read(false);
	cancel_read(true);
	return 0;
}

static int test_cancel_done(void)
{
	struct rec nop, cancel;
	struct chio_loop loop;

	loop_init(&loop, 8);
	rec_init(&nop, rec_complete);
	rec_init(&cancel, rec_complete);
	arm_nop(&loop, &nop.op);
	drain(&loop, &nop.op);
	io_uring_prep_cancel(get_sqe(&loop, &cancel.op), &nop.op, 0);
	drain(&loop, &cancel.op);
	CHECK_EQ(nop.calls, 1);
	CHECK_EQ(nop.res, 0);
	CHECK_EQ(cancel.calls, 1);
	CHECK_EQ(cancel.res, -ENOENT);
	chio_loop_exit(&loop);
	return 0;
}

static int test_cancel_fd(void)
{
	struct rec readers[2], cancel;
	struct chio_loop loop;
	char buf[2][8];
	int fds[2], ret = 0;
	size_t i;

	make_pipe(fds);
	loop_init(&loop, 8);
	for (i = 0; i < ARRAY_SIZE(readers); i++) {
		rec_init(&readers[i], rec_complete);
		arm_read(&loop, &readers[i].op, fds[0], buf[i], sizeof(buf[i]));
	}
	rec_init(&cancel, rec_complete);
	io_uring_prep_cancel_fd(get_sqe(&loop, &cancel.op), fds[0],
				IORING_ASYNC_CANCEL_ALL);
	drain(&loop, &cancel.op);
	if (cancel.res == -EINVAL) {
		ret = SKIP;
	} else {
		CHECK_EQ(cancel.res, ARRAY_SIZE(readers));
		for (i = 0; i < ARRAY_SIZE(readers); i++) {
			drain(&loop, &readers[i].op);
			CHECK_EQ(readers[i].calls, 1);
			CHECK_EQ(readers[i].res, -ECANCELED);
		}
	}
	chio_loop_exit(&loop);
	close_pipe(fds);
	return ret;
}

static int test_cancel_any(void)
{
	struct __kernel_timespec ts = { .tv_sec = 10 };
	struct rec ops[3], cancel;
	struct chio_loop loop;
	int fds[2], ret = 0;
	char buf[8];
	size_t i;

	make_pipe(fds);
	loop_init(&loop, 8);
	for (i = 0; i < ARRAY_SIZE(ops); i++)
		rec_init(&ops[i], rec_complete);
	rec_init(&cancel, rec_complete);
	arm_read(&loop, &ops[0].op, fds[0], buf, sizeof(buf));
	io_uring_prep_poll_add(get_sqe(&loop, &ops[1].op), fds[0], POLLIN);
	io_uring_prep_timeout(get_sqe(&loop, &ops[2].op), &ts, 0, 0);
	CHECK_EQ(chio_loop_run_once(&loop, false), 0);
	io_uring_prep_cancel(get_sqe(&loop, &cancel.op), nullptr,
			     IORING_ASYNC_CANCEL_ANY);
	drain(&loop, &cancel.op);
	if (cancel.res == -EINVAL) {
		ret = SKIP;
	} else {
		CHECK_EQ(cancel.res, ARRAY_SIZE(ops));
		for (i = 0; i < ARRAY_SIZE(ops); i++) {
			drain(&loop, &ops[i].op);
			CHECK_EQ(ops[i].calls, 1);
			CHECK_EQ(ops[i].res, -ECANCELED);
		}
	}
	chio_loop_exit(&loop);
	close_pipe(fds);
	return ret;
}

struct heap_op {
	struct chio_op op;
	int *freed;
};

static void heap_complete(struct chio_loop *, struct chio_op *op,
			  const struct io_uring_cqe *cqe)
{
	struct heap_op *heap = container_of(op, struct heap_op, op);

	CHECK_EQ(cqe->res, 0);
	(*heap->freed)++;
	memset(heap, 0xa5, sizeof(*heap));
	free(heap);
}

static int test_free_in_callback(void)
{
	struct heap_op *heap;
	struct chio_loop loop;
	int freed = 0, i;

	loop_init(&loop, 8);
	for (i = 0; i < 8; i++) {
		heap = calloc(1, sizeof(*heap));
		CHECK(heap);
		chio_op_init(&heap->op, heap_complete);
		heap->freed = &freed;
		arm_nop(&loop, &heap->op);
	}
	while (freed < 8)
		CHECK(chio_loop_run_once(&loop, true) >= 0);
	chio_loop_exit(&loop);
	return 0;
}

static int test_stop(void)
{
	struct rec reader, stopper;
	struct chio_loop loop;
	char buf[8];
	int fds[2];

	make_pipe(fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	rec_init(&stopper, stop_complete);
	arm_read(&loop, &reader.op, fds[0], buf, sizeof(buf));
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
	close_pipe(fds);
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
	char buf[8] = {};
	struct rec rec;
	int fds[2];

	make_pipe(fds);
	alarms = 0;
	alarm_fd = -1;
	CHECK_EQ(sigemptyset(&sa.sa_mask), 0);
	CHECK_EQ(sigaction(SIGALRM, &sa, &old), 0);
	loop_init(&loop, 8);
	rec_init(&rec, stop_complete);
	arm_read(&loop, &rec.op, fds[0], buf, 1);
	CHECK_EQ(chio_loop_run_once(&loop, false), 0);

	set_alarm(20000);
	CHECK_EQ(chio_loop_run_once(&loop, true), 0);
	set_alarm(0);
	CHECK(alarms >= 1);
	CHECK(rec.op.pending);

	alarm_fd = fds[1];
	set_alarm(20000);
	CHECK_EQ(chio_loop_run(&loop), 0);
	set_alarm(0);
	CHECK(alarms >= 2);
	CHECK_EQ(rec.calls, 1);
	CHECK_EQ(rec.res, 1);
	CHECK_EQ(buf[0], 'x');
	CHECK(!rec.op.pending);

	chio_loop_exit(&loop);
	alarm_fd = -1;
	CHECK_EQ(sigaction(SIGALRM, &old, nullptr), 0);
	close_pipe(fds);
	return 0;
}

static int test_exit_pending(void)
{
	struct chio_loop loop;
	struct rec rec;
	char buf[8];
	int fds[2];

	make_pipe(fds);
	loop_init(&loop, 8);
	rec_init(&rec, rec_complete);
	arm_read(&loop, &rec.op, fds[0], buf, sizeof(buf));
	CHECK_EQ(chio_loop_run_once(&loop, false), 0);
	chio_loop_exit(&loop);
	CHECK_EQ(rec.calls, 0);
	CHECK(rec.op.pending);
	close_pipe(fds);
	return 0;
}

struct test {
	const char *name;
	int (*fn)(void);
};

#define TEST(name) { #name, test_##name }

static const struct test tests[] = {
	TEST(op_init),
	TEST(init_exit),
	TEST(init_params),
	TEST(init_errors),
	TEST(nop),
	TEST(nowait),
	TEST(batch),
	TEST(bounded_dispatch),
	TEST(resubmit),
	TEST(sq_full),
	TEST(submit_error),
	TEST(run_error),
	TEST(submit_retry),
	TEST(double_arm),
	TEST(untracked),
	TEST(op_error),
	TEST(pipe_io),
	TEST(linked_timeout),
	TEST(multishot),
	TEST(multishot_cancel),
	TEST(cancel),
	TEST(cancel_done),
	TEST(cancel_fd),
	TEST(cancel_any),
	TEST(free_in_callback),
	TEST(stop),
	TEST(eintr),
	TEST(exit_pending),
};

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
	for (i = 0; i < ARRAY_SIZE(tests); i++)
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
		for (i = 0; i < ARRAY_SIZE(tests); i++)
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
	for (i = 0; i < ARRAY_SIZE(tests); i++)
		run_test(&tests[i]);
	return 0;
}
