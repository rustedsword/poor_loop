/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poll.h>
#include <poor_loop.h>
#include <poor_loop_fd.h>
#include <poor_stdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

/*
 * Poll a timerfd until it triggers TRIGGERS times. A poll completes only once,
 * so each trigger reads the timerfd to clear it and polls it again. The poll
 * is attached to an fd group, which closes the timerfd after the last poll.
 */
constexpr unsigned TRIGGERS = 3;
constexpr long PERIOD_NS = 200'000'000;
constexpr uint64_t DRAIN_TIMEOUT_NS = 1'000'000'000;

poor_loop_fd_declare(ticker_file);

struct ticker {
	ticker_file file;
	struct poor_loop_op poll_op;
	unsigned triggers;
	bool failed;
};

poor_loop_group_attach(ticker_file, struct ticker, file, poll_op, on_poll);

static struct ticker *ticker_of(ticker_file *file)
{
	return container_of(file, struct ticker, file);
}

static void poll_timer(struct poor_loop *loop, struct ticker *t)
{
	io_uring_prep_poll_add(poor_loop_group_get_sqe(loop, &t->file, &t->poll_op), t->file.fd, POLLIN);
}

POOR_LOOP_GROUP_OP(on_poll, loop, file, op, cqe)
{
	struct ticker *t = ticker_of(file);
	uint64_t expirations;

	if (cqe->res < 0) {
		printerrln("poll: ", strerror(-cqe->res));
		t->failed = true;
		return;
	}
	if (read(file->fd, &expirations, sizeof(expirations)) > 0)
		println("trigger ", ++t->triggers, ", expirations: ", expirations);
	if (t->triggers < TRIGGERS)
		poll_timer(loop, t);
}

POOR_LOOP_FD_DONE(ticker_file, loop, file)
{
	println("timerfd closed");
	poor_loop_stop(loop);
}

int main(void)
{
	struct itimerspec its = { .it_interval.tv_nsec = PERIOD_NS, .it_value.tv_nsec = PERIOD_NS };
	struct ticker t = { .poll_op = POOR_LOOP_OP_INIT(on_poll) };
	struct io_uring_params params = {};
	struct poor_loop loop;
	int fd, ret;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (fd < 0) {
		perror("timerfd_create");
		return 1;
	}
	ret = poor_loop_init(&loop, 8, &params);
	if (ret) {
		printerrln("poor_loop_init: ", strerror(-ret));
		close(fd);
		return 1;
	}
	timerfd_settime(fd, 0, &its, nullptr);
	poor_loop_fd_init(&t.file, fd);
	poll_timer(&loop, &t);
	ret = poor_loop_run(&loop);
	if (!ret)
		ret = poor_loop_drain(&loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	if (ret)
		printerrln("loop: ", strerror(-ret));
	poor_loop_exit(&loop);
	return ret || t.failed;
}
