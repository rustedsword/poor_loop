/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <fcntl.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <poor_stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

/*
 * Two threads with a loop each talk over a pipe per direction: the main
 * thread sends a message and the other thread replies. Every pipe end is an
 * fd group that closes the end after its last read or write, and a loop stops
 * once both of its ends are closed.
 */
constexpr uint64_t DRAIN_TIMEOUT_NS = 1'000'000'000;

poor_loop_fd_declare(pipe_end);

struct peer {
	struct poor_loop loop;
	pipe_end in, out;
	struct poor_loop_op read_op, write_op;
	const char *name, *send, *reply;
	unsigned open;
	bool failed;
	char buf[64];
};

poor_loop_group_attach(pipe_end, struct peer, in, read_op, on_read);
poor_loop_group_attach(pipe_end, struct peer, out, write_op, on_write);

static struct peer *peer_of(struct poor_loop *loop)
{
	return container_of(loop, struct peer, loop);
}

static bool check(struct peer *p, const char *what, int res)
{
	if (res > 0)
		return true;
	printerrln(p->name, ": ", what, ": ", res ? strerror(-res) : "end of file");
	p->failed = true;
	return false;
}

static void start_write(struct peer *p, const char *msg)
{
	io_uring_prep_write(poor_loop_group_get_sqe(&p->loop, &p->out, &p->write_op), p->out.fd, msg, strlen(msg), -1);
}

POOR_LOOP_GROUP_OP(on_read, loop, in, op, cqe)
{
	struct peer *p = peer_of(loop);

	if (check(p, "read", cqe->res)) {
		println(p->name, ": got ", fmt_p(p->buf, cqe->res));
		if (p->reply)
			start_write(p, p->reply);
	}
	if (poor_loop_group_release(&p->out))
		poor_loop_group_call_done(pipe_end, loop, &p->out);
}

POOR_LOOP_GROUP_OP(on_write, loop, out, op, cqe)
{
	check(peer_of(loop), "write", cqe->res);
}

POOR_LOOP_FD_DONE(pipe_end, loop, end)
{
	if (!--peer_of(loop)->open)
		poor_loop_stop(loop);
}

static void close_ends(struct peer *p)
{
	close(p->in.fd);
	close(p->out.fd);
}

static void *run(void *arg)
{
	struct io_uring_params params = {};
	struct peer *p = arg;
	int ret;

	ret = poor_loop_init(&p->loop, 8, &params);
	if (ret) {
		printerrln(p->name, ": poor_loop_init: ", strerror(-ret));
		close_ends(p);
		p->failed = true;
		return nullptr;
	}
	/* The reply is written after the read, so hold the write end until then */
	poor_loop_group_acquire(&p->out);
	if (p->send)
		start_write(p, p->send);
	io_uring_prep_read_array(poor_loop_group_get_sqe(&p->loop, &p->in, &p->read_op), p->in.fd, p->buf, -1);
	ret = poor_loop_run(&p->loop);
	if (!ret)
		ret = poor_loop_drain(&p->loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	if (ret) {
		printerrln(p->name, ": loop: ", strerror(-ret));
		p->failed = true;
	}
	poor_loop_exit(&p->loop);
	return nullptr;
}

int main(void)
{
	int request[2], reply[2], ret;
	pthread_t thread;

	if (pipe2(request, O_CLOEXEC)) {
		perror("pipe2");
		return 1;
	}
	if (pipe2(reply, O_CLOEXEC)) {
		perror("pipe2");
		close(request[0]);
		close(request[1]);
		return 1;
	}
	struct peer ping = {
		.in = POOR_LOOP_FD_INIT(reply[0]),
		.out = POOR_LOOP_FD_INIT(request[1]),
		.read_op = POOR_LOOP_OP_INIT(on_read),
		.write_op = POOR_LOOP_OP_INIT(on_write),
		.name = "main",
		.send = "ping",
		.open = 2,
	};
	struct peer pong = {
		.in = POOR_LOOP_FD_INIT(request[0]),
		.out = POOR_LOOP_FD_INIT(reply[1]),
		.read_op = POOR_LOOP_OP_INIT(on_read),
		.write_op = POOR_LOOP_OP_INIT(on_write),
		.name = "thread",
		.reply = "pong",
		.open = 2,
	};
	ret = pthread_create(&thread, nullptr, run, &pong);
	if (ret) {
		printerrln("pthread_create: ", strerror(ret));
		close_ends(&ping);
		close_ends(&pong);
		return 1;
	}
	run(&ping);
	pthread_join(thread, nullptr);
	return ping.failed || pong.failed;
}
