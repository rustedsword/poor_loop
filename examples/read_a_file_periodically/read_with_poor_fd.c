/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <fcntl.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <poor_stdio.h>
#include <string.h>

/*
 * Read a file a few times with a pause between the reads: open it, read it
 * from the start, wait and read it again. The reads and the timer are attached
 * to an fd group, so the file stays open while one of them is pending and the
 * group closes it after the last read.
 *
 * Usage: read_with_poor_fd [FILE]
 */
constexpr unsigned READS = 3;
constexpr uint64_t PERIOD_NS = 1'000'000'000;
constexpr uint64_t DRAIN_TIMEOUT_NS = 1'000'000'000;

poor_loop_fd_declare(reader_file);

struct reader {
	reader_file file;
	struct poor_loop_op open_op, read_op;
	struct poor_loop_timer delay;
	const char *path;
	unsigned reads;
	bool failed;
	char buf[4096];
};

poor_loop_group_attach(reader_file, struct reader, file, read_op, read_done);
poor_loop_group_attach(reader_file, struct reader, file, delay, delay_fire);

static struct reader *reader_of(reader_file *file)
{
	return container_of(file, struct reader, file);
}

static void start_read(struct poor_loop *loop, struct reader *r)
{
	io_uring_prep_read_array(poor_loop_group_get_sqe(loop, &r->file, &r->read_op), r->file.fd, r->buf, 0);
}

static void open_done(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe)
{
	struct reader *r = container_of(op, struct reader, open_op);

	if (cqe->res < 0) {
		printerrln(r->path, ": open: ", strerror(-cqe->res));
		r->failed = true;
		poor_loop_stop(loop);
		return;
	}
	poor_loop_fd_init(&r->file, cqe->res);
	start_read(loop, r);
}

POOR_LOOP_GROUP_OP(read_done, loop, file, op, cqe)
{
	struct reader *r = reader_of(file);

	if (cqe->res < 0) {
		printerrln(r->path, ": read: ", strerror(-cqe->res));
		r->failed = true;
		return;
	}
	r->reads++;
	print("read ", r->reads, ": ", fmt_p(r->buf, cqe->res));
	if (r->reads < READS)
		poor_loop_group_timer_arm(loop, file, &r->delay, poor_loop_now() + PERIOD_NS);
}

POOR_LOOP_GROUP_TIMER(delay_fire, loop, file, timer)
{
	start_read(loop, reader_of(file));
}

POOR_LOOP_FD_DONE(reader_file, loop, file)
{
	println("closed ", reader_of(file)->path);
	poor_loop_stop(loop);
}

int main(int argc, char **argv)
{
	struct reader r = {
		.open_op = POOR_LOOP_OP_INIT(open_done),
		.read_op = POOR_LOOP_OP_INIT(read_done),
		.delay = POOR_LOOP_TIMER_INIT(delay_fire),
		.path = argc > 1 ? argv[1] : "/proc/uptime",
	};
	struct io_uring_params params = {};
	struct poor_loop loop;
	int ret;

	ret = poor_loop_init(&loop, 8, &params);
	if (ret) {
		printerrln("poor_loop_init: ", strerror(-ret));
		return 1;
	}
	io_uring_prep_openat(poor_loop_get_sqe(&loop, &r.open_op), AT_FDCWD, r.path, O_RDONLY | O_CLOEXEC, 0);
	ret = poor_loop_run(&loop);
	if (!ret)
		ret = poor_loop_drain(&loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	if (ret)
		printerrln("loop: ", strerror(-ret));
	poor_loop_exit(&loop);
	return ret || r.failed;
}
