/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <fcntl.h>
#include <poor_loop.h>
#include <poor_loop_group.h>
#include <poor_stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK 64

/*
 * Read a file with one group: two reads in struct job, a timer, and allocated
 * requests started by the timer and by the statx callback. When the group is
 * done, a plain op closes the file.
 */
poor_loop_group_declare(job_io);

struct job {
	job_io io;
	struct poor_loop_op first, second;
	struct poor_loop_timer delay;
	struct poor_loop_op close_op;
	int fd;
	bool failed;
	char buf[2][CHUNK];
};

struct read_req {
	struct poor_loop_op op;
	job_io *io;
	char buf[CHUNK];
};

struct stat_req {
	struct poor_loop_op op;
	job_io *io;
	struct statx stx;
};

poor_loop_group_attach(job_io, struct job, io, first, first_done);
poor_loop_group_attach(job_io, struct job, io, second, second_done);
poor_loop_group_attach(job_io, struct job, io, delay, delay_fire);
poor_loop_group_attach(job_io, struct read_req, io, op, read_done);
poor_loop_group_attach(job_io, struct read_req, io, op, tail_done);
poor_loop_group_attach(job_io, struct stat_req, io, op, stat_done);

static struct job *job_of(job_io *io)
{
	return container_of(io, struct job, io);
}

static void report(job_io *io, const char *name, int res)
{
	println(name, ": ", res);
	job_of(io)->failed |= res < 0;
}

static void start_read(struct poor_loop *loop, job_io *io, poor_loop_complete_fn *done, uint64_t offset)
{
	struct read_req *req = malloc(sizeof(*req));

	if (!req) {
		perror("malloc");
		job_of(io)->failed = true;
		return;
	}
	poor_loop_op_init(&req->op, done);
	req->io = io;
	io_uring_prep_read_array(poor_loop_group_get_sqe(loop, io, &req->op), job_of(io)->fd, req->buf, offset);
}

static void start_stat(struct poor_loop *loop, job_io *io)
{
	struct stat_req *req = malloc(sizeof(*req));

	if (!req) {
		perror("malloc");
		job_of(io)->failed = true;
		return;
	}
	poor_loop_op_init(&req->op, stat_done);
	req->io = io;
	auto sqe = poor_loop_group_get_sqe(loop, io, &req->op);
	io_uring_prep_statx(sqe, job_of(io)->fd, "", AT_EMPTY_PATH, STATX_SIZE, &req->stx);
}

POOR_LOOP_GROUP_OP(first_done, loop, io, op, cqe)
{
	report(io, "first read", cqe->res);
}

POOR_LOOP_GROUP_OP(second_done, loop, io, op, cqe)
{
	report(io, "second read", cqe->res);
}

POOR_LOOP_GROUP_OP(read_done, loop, io, op, cqe)
{
	report(io, "dynamic read", cqe->res);
	free(container_of(op, struct read_req, op));
}

POOR_LOOP_GROUP_OP(tail_done, loop, io, op, cqe)
{
	report(io, "tail read", cqe->res);
	free(container_of(op, struct read_req, op));
}

POOR_LOOP_GROUP_OP(stat_done, loop, io, op, cqe)
{
	struct stat_req *req = container_of(op, struct stat_req, op);
	uint64_t size = req->stx.stx_size;

	report(io, "statx", cqe->res);
	free(req);
	if (cqe->res < 0)
		return;
	println("size: ", size);
	start_read(loop, io, tail_done, size > CHUNK ? size - CHUNK : 0);
}

POOR_LOOP_GROUP_TIMER(delay_fire, loop, io, timer)
{
	println("timer fired");
	start_read(loop, io, read_done, 2 * CHUNK);
	start_stat(loop, io);
}

static void close_done(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	println("close: ", cqe->res);
	container_of(op, struct job, close_op)->failed |= cqe->res < 0;
	poor_loop_stop(loop);
}

POOR_LOOP_GROUP_DONE(job_io, loop, io)
{
	struct job *job = job_of(io);
	struct io_uring_sqe *sqe = poor_loop_get_sqe_or_submit(loop, &job->close_op);

	io_uring_prep_close(sqe, job->fd);
	sqe->flags |= IOSQE_ASYNC;
}

int main(int argc, char **argv)
{
	struct job job = {
		.io = POOR_LOOP_GROUP_INIT,
		.first = POOR_LOOP_OP_INIT(first_done),
		.second = POOR_LOOP_OP_INIT(second_done),
		.delay = POOR_LOOP_TIMER_INIT(delay_fire),
		.close_op = POOR_LOOP_OP_INIT(close_done),
	};
	struct io_uring_params params = {};
	struct poor_loop loop;
	int ret;

	job.fd = open(argc > 1 ? argv[1] : "/etc/os-release", O_RDONLY | O_CLOEXEC);
	if (job.fd < 0) {
		perror("open");
		return 1;
	}
	ret = poor_loop_init(&loop, 8, &params);
	if (ret) {
		printerrln("poor_loop_init: ", strerror(-ret));
		close(job.fd);
		return 1;
	}
	io_uring_prep_read_array(poor_loop_group_get_sqe(&loop, &job.io, &job.first), job.fd, job.buf[0], 0);
	io_uring_prep_read_array(poor_loop_group_get_sqe(&loop, &job.io, &job.second), job.fd, job.buf[1], CHUNK);
	poor_loop_group_timer_arm(&loop, &job.io, &job.delay, poor_loop_now() + 10'000'000);
	ret = poor_loop_run(&loop);
	poor_loop_exit(&loop);
	return ret != 0 || job.failed;
}
