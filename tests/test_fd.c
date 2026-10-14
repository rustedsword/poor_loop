/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_loop_fd.h>

#include "harness.h"

static char logged[128];
static int logs;

static void log_record(struct poor_loop *, const char *fmt, va_list ap)
{
	vsnprintf_array(logged, fmt, ap);
	logs++;
}

static bool is_open(int fd)
{
	return fcntl(fd, F_GETFD) >= 0;
}

static void check_close_queued(struct poor_loop *loop, int fd, unsigned sqe_flags)
{
	struct io_uring_sq *sq = &poor_loop_ring(loop)->sq;
	const struct io_uring_sqe *sqe = &sq->sqes[(sq->sqe_tail - 1) & sq->ring_mask];

	CHECK_EQ(sqe->opcode, IORING_OP_CLOSE);
	CHECK_EQ(sqe->fd, fd);
	CHECK_EQ(sqe->flags, sqe_flags);
	CHECK_EQ(sqe->user_data, 0);
}

/* The loop ignores the CQE of the close, so wait for the fd to go away. */
static void wait_closed(struct poor_loop *loop, int fd)
{
	while (is_open(fd))
		CHECK_EQ(poor_loop_run_once(loop), 0);
	CHECK_EQ(errno, EBADF);
}

poor_loop_fd_declare(pipe_file);

struct reader {
	pipe_file file;
	struct poor_loop_op first, second;
	unsigned completed, calls;
	char buf[2];
};

poor_loop_group_attach(pipe_file, struct reader, file, first, first_done);
poor_loop_group_attach(pipe_file, struct reader, file, second, second_done);

static void read_done(pipe_file *file, const struct io_uring_cqe *cqe)
{
	CHECK_EQ(cqe->res, 1);
	CHECK(is_open(file->fd));
	container_of(file, struct reader, file)->completed++;
}

POOR_LOOP_GROUP_OP(first_done, loop, file, op, cqe)
{
	read_done(file, cqe);
}

POOR_LOOP_GROUP_OP(second_done, loop, file, op, cqe)
{
	read_done(file, cqe);
}

POOR_LOOP_FD_DONE(pipe_file, loop, file)
{
	struct reader *r = container_of(file, struct reader, file);

	CHECK(!r->first.pending && !r->second.pending);
	CHECK_EQ(file->fd, -1);
	r->calls++;
	poor_loop_stop(loop);
}

static int test_fd_init(void)
{
	static struct reader r = { .file = POOR_LOOP_FD_INIT(3) };
	unsigned evaluated = 0;

	CHECK_EQ(r.file.group.pending, 0);
	CHECK_EQ(r.file.fd, 3);
	memset(&r.file, 0xa5, sizeof(r.file));
	poor_loop_fd_init((evaluated++, &r.file), 4);
	CHECK_EQ(evaluated, 1);
	CHECK_EQ(r.file.group.pending, 0);
	CHECK_EQ(r.file.fd, 4);
	return 0;
}

static int test_fd_close_flags(void)
{
	struct poor_loop loop;
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	poor_loop_close_fd_or_submit(&loop, fds[0], 0);
	check_close_queued(&loop, fds[0], 0);
	wait_closed(&loop, fds[0]);
	poor_loop_exit(&loop);
	CHECK_EQ(close(fds[1]), 0);
	return 0;
}

static int test_fd_close_after_ops(void)
{
	struct reader r = {
		.first = POOR_LOOP_OP_INIT(first_done),
		.second = POOR_LOOP_OP_INIT(second_done),
	};
	struct poor_loop loop;
	int fds[2];

	make_pipe(&fds);
	poor_loop_fd_init(&r.file, fds[0]);
	CHECK_EQ(write(fds[1], "ab", 2), 2);
	loop_init(&loop, 8);
	io_uring_prep_read_array(poor_loop_group_get_sqe(&loop, &r.file, &r.first), r.file.fd, array_ptr(&r.buf[0]), 0);
	io_uring_prep_read_array(poor_loop_group_get_sqe(&loop, &r.file, &r.second), r.file.fd, array_ptr(&r.buf[1]), 0);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(r.completed, 2);
	CHECK_EQ(r.calls, 1);
	check_close_queued(&loop, fds[0], IOSQE_ASYNC);
	wait_closed(&loop, fds[0]);
	poor_loop_exit(&loop);
	CHECK_EQ(close(fds[1]), 0);
	return 0;
}

static int test_fd_release_sq_full(void)
{
	struct reader r = {
		.first = POOR_LOOP_OP_INIT(first_done),
		.second = POOR_LOOP_OP_INIT(second_done),
	};
	struct poor_loop loop;
	int fds[2];

	make_pipe(&fds);
	poor_loop_fd_init(&r.file, fds[0]);
	loop_init(&loop, 2);
	io_uring_prep_nop(poor_loop_get_untracked_sqe(&loop));
	io_uring_prep_nop(poor_loop_get_untracked_sqe(&loop));
	poor_loop_group_acquire(&r.file);
	CHECK(poor_loop_group_release(&r.file));
	CHECK(is_open(fds[0]));
	poor_loop_group_call_done(pipe_file, &loop, &r.file);
	CHECK_EQ(r.calls, 1);
	check_close_queued(&loop, fds[0], IOSQE_ASYNC);
	wait_closed(&loop, fds[0]);
	poor_loop_exit(&loop);
	CHECK_EQ(close(fds[1]), 0);
	return 0;
}

static int test_fd_sync_close(void)
{
	struct reader r = {
		.first = POOR_LOOP_OP_INIT(first_done),
		.second = POOR_LOOP_OP_INIT(second_done),
	};
	struct io_uring_params p;
	struct poor_loop loop;
	int fds[2];

	make_pipe(&fds);
	poor_loop_fd_init(&r.file, fds[0]);
	setup_params(&p);
	p.flags |= IORING_SETUP_R_DISABLED;
	CHECK_EQ(poor_loop_init(&loop, 2, &p), 0);
	io_uring_prep_nop(poor_loop_get_untracked_sqe(&loop));
	io_uring_prep_nop(poor_loop_get_untracked_sqe(&loop));
	logs = 0;
	poor_loop_log_function_set(log_record);
	poor_loop_group_call_done(pipe_file, &loop, &r.file);
	poor_loop_log_function_set(nullptr);
	CHECK_EQ(r.calls, 1);
	CHECK(!is_open(fds[0]));
	CHECK_EQ(logs, 2);
	concat_vla(expected, "No SQE to close fd ", fds[0], ": ", strerror(EBADFD), ": closing synchronously");
	CHECK(!strcmp(logged, expected));
	poor_loop_exit(&loop);
	CHECK_EQ(close(fds[1]), 0);
	return 0;
}

poor_loop_fd_declare(owned_file);

struct owned {
	owned_file file;
	struct poor_loop_op nop;
	int fd;
	bool *freed;
};

poor_loop_group_attach(owned_file, struct owned, file, nop, owned_nop_done);

POOR_LOOP_GROUP_OP(owned_nop_done, loop, file, op, cqe)
{
	CHECK_EQ(cqe->res, 0);
}

POOR_LOOP_FD_DONE(owned_file, loop, file)
{
	struct owned *o = container_of(file, struct owned, file);
	bool *freed = o->freed;

	CHECK_EQ(file->fd, -1);
	check_close_queued(loop, o->fd, IOSQE_ASYNC);
	memset(o, 0xa5, sizeof(*o));
	free(o);
	*freed = true;
}

static int test_fd_free_in_done(void)
{
	struct owned *o = malloc(sizeof(*o));
	struct poor_loop loop;
	bool freed = false;
	int fds[2];

	CHECK(o);
	make_pipe(&fds);
	*o = (struct owned){
		.file = POOR_LOOP_FD_INIT(fds[0]),
		.nop = POOR_LOOP_OP_INIT(owned_nop_done),
		.fd = fds[0],
		.freed = &freed,
	};
	loop_init(&loop, 8);
	io_uring_prep_nop(poor_loop_group_get_sqe(&loop, &o->file, &o->nop));
	while (!freed)
		CHECK_EQ(poor_loop_run_once(&loop), 0);
	wait_closed(&loop, fds[0]);
	poor_loop_exit(&loop);
	CHECK_EQ(close(fds[1]), 0);
	return 0;
}

static const struct test tests[] = {
	TEST(fd_init),
	TEST(fd_close_flags),
	TEST(fd_close_after_ops),
	TEST(fd_release_sq_full),
	TEST(fd_sync_close),
	TEST(fd_free_in_done),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
