/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <poor_array.h>
#include <poor_loop.h>
#include <poor_loop_group.h>
#include <poor_stdio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

/*
 * Run an application that uses a third-party library until SIGINT, SIGTERM or,
 * if given, SECONDS pass, then shut it down cleanly with poor_loop_drain().
 *
 * Usage: drain [SECONDS]
 */
constexpr long TICK_NS = 500'000'000;
constexpr uint64_t SHUTDOWN_TIMEOUT_NS = 2'000'000'000;
constexpr uint64_t DRAIN_TIMEOUT_NS = 2'000'000'000;

/*
 * A stand-in for a library with its own event loop. lib_get_fd(),
 * lib_dispatch() and lib_flush() work like wl_event_loop_get_fd(),
 * wl_event_loop_dispatch() with a zero timeout and wl_display_flush_clients().
 * Its only event source is a timer, and closing it takes a handshake: after
 * lib_close(), lib_flush() writes a goodbye, and the next event is the reply.
 */
enum lib_state { LIB_OPEN, LIB_CLOSING, LIB_BYE_SENT, LIB_CLOSED };

struct lib {
	int fd;
	enum lib_state state;
	uint64_t ticks;
	bool dirty;
};

poor_loop_group_declare(app_io);

struct app {
	app_io io;
	struct lib lib;
	struct poor_loop_op lib_op, signal_op;
	struct poor_loop_timer timer, shutdown;
	struct signalfd_siginfo siginfo;
	sigset_t signals;
	int signal_fd, status;
	bool timed_out, down;
};

poor_loop_group_attach(app_io, struct app, io, lib_op, on_lib);
poor_loop_group_attach(app_io, struct app, io, signal_op, on_signal);
poor_loop_group_attach(app_io, struct app, io, timer, on_timer);

static int lib_init(struct lib *lib)
{
	struct itimerspec its = { .it_interval.tv_nsec = TICK_NS, .it_value.tv_nsec = TICK_NS };

	lib->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (lib->fd < 0)
		return -errno;
	timerfd_settime(lib->fd, 0, &its, nullptr);
	lib->state = LIB_OPEN;
	lib->ticks = 0;
	lib->dirty = false;
	return 0;
}

static int lib_get_fd(const struct lib *lib)
{
	return lib->fd;
}

static void lib_dispatch(struct lib *lib)
{
	uint64_t expirations;

	if (read(lib->fd, &expirations, sizeof(expirations)) <= 0)
		return;
	if (lib->state == LIB_BYE_SENT) {
		println("lib: closed");
		lib->state = LIB_CLOSED;
	} else {
		lib->ticks += expirations;
		lib->dirty = true;
	}
}

static void lib_flush(struct lib *lib)
{
	if (lib->dirty)
		println("lib: tick ", lib->ticks);
	lib->dirty = false;
	if (lib->state == LIB_CLOSING) {
		println("lib: goodbye");
		lib->state = LIB_BYE_SENT;
	}
}

static void lib_close(struct lib *lib)
{
	lib->state = LIB_CLOSING;
}

static bool lib_closed(const struct lib *lib)
{
	return lib->state == LIB_CLOSED;
}

static void lib_destroy(struct lib *lib)
{
	close(lib->fd);
}

static bool check(struct app *app, struct poor_loop *loop, const char *name, int res)
{
	if (res >= 0)
		return true;
	if (res == -ECANCELED) {
		println(name, ": cancelled");
	} else {
		printerrln(name, ": ", strerror(-res));
		app->status = 1;
		poor_loop_stop(loop);
	}
	return false;
}

static struct app *app_of(app_io *io)
{
	return container_of(io, struct app, io);
}

static void cancel(struct poor_loop *loop, struct poor_loop_op *op)
{
	if (op->pending)
		io_uring_prep_cancel(poor_loop_get_untracked_sqe(loop), op, 0);
}

static void poll_lib(struct poor_loop *loop, struct app *app)
{
	io_uring_prep_poll_add(poor_loop_group_get_sqe(loop, &app->io, &app->lib_op), lib_get_fd(&app->lib), POLLIN);
}

POOR_LOOP_GROUP_OP(on_lib, loop, io, op, cqe)
{
	struct app *app = app_of(io);

	if (!check(app, loop, "lib poll", cqe->res))
		return;
	lib_dispatch(&app->lib);
	if (!lib_closed(&app->lib) && !app->timed_out)
		poll_lib(loop, app);
}

POOR_LOOP_GROUP_OP(on_signal, loop, io, op, cqe)
{
	struct app *app = app_of(io);

	if (check(app, loop, "signalfd", cqe->res)) {
		println("signalfd: ", strsignal(app->siginfo.ssi_signo));
		poor_loop_stop(loop);
	}
	io_uring_prep_close(poor_loop_get_untracked_sqe(loop), app->signal_fd);
	sigprocmask(SIG_UNBLOCK, &app->signals, nullptr);
}

POOR_LOOP_GROUP_TIMER(on_timer, loop, io, timer)
{
	println("timer: expired");
	poor_loop_stop(loop);
}

POOR_LOOP_GROUP_DONE(app_io, loop, io)
{
	struct app *app = app_of(io);

	poor_loop_timer_disarm(&app->shutdown);
	app->down = true;
	println("app: down");
}

static void on_shutdown(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	struct app *app = container_of(timer, struct app, shutdown);

	printerrln("lib: no reply");
	app->status = 1;
	app->timed_out = true;
	cancel(loop, &app->lib_op);
}

[[nodiscard]] static int app_start(struct app *app, struct poor_loop *loop, uint64_t seconds)
{
	int ret = lib_init(&app->lib);

	if (ret)
		return ret;
	sigemptyset(&app->signals);
	sigaddset(&app->signals, SIGINT);
	sigaddset(&app->signals, SIGTERM);
	sigprocmask(SIG_BLOCK, &app->signals, nullptr);
	app->signal_fd = signalfd(-1, &app->signals, SFD_CLOEXEC);
	if (app->signal_fd < 0) {
		ret = -errno;
		lib_destroy(&app->lib);
		return ret;
	}
	poor_loop_group_init(&app->io);
	poor_loop_op_init(&app->lib_op, on_lib);
	poor_loop_op_init(&app->signal_op, on_signal);
	poor_loop_timer_init(&app->timer, on_timer);
	poor_loop_timer_init(&app->shutdown, on_shutdown);
	app->status = 0;
	app->timed_out = false;
	app->down = false;
	poll_lib(loop, app);
	io_uring_prep_read_array(poor_loop_group_get_sqe(loop, &app->io, &app->signal_op), app->signal_fd,
				 array_ptr(&app->siginfo), -1);
	if (seconds)
		poor_loop_group_timer_arm(loop, &app->io, &app->timer, poor_loop_now() + seconds * 1'000'000'000);
	return 0;
}

static void app_idle(struct app *app)
{
	lib_flush(&app->lib);
}

static void app_stop(struct app *app, struct poor_loop *loop)
{
	poor_loop_timer_arm(loop, &app->shutdown, poor_loop_now() + SHUTDOWN_TIMEOUT_NS);
	cancel(loop, &app->signal_op);
	lib_close(&app->lib);
	if (poor_loop_group_timer_disarm(&app->io, &app->timer))
		poor_loop_group_call_done(app_io, loop, &app->io);
}

static void app_destroy(struct app *app)
{
	lib_destroy(&app->lib);
}

int main(int argc, char **argv)
{
	uint64_t seconds = argc > 1 ? strtoull(argv[1], nullptr, 10) : 0;
	struct io_uring_params params = {};
	struct poor_loop loop;
	struct app app;
	int ret;

	ret = poor_loop_init(&loop, 8, &params);
	if (ret) {
		printerrln("poor_loop_init: ", strerror(-ret));
		return 1;
	}
	ret = app_start(&app, &loop, seconds);
	if (ret) {
		printerrln("app_start: ", strerror(-ret));
		poor_loop_exit(&loop);
		return 1;
	}
	do {
		app_idle(&app);
		ret = poor_loop_run_once(&loop);
	} while (!ret && !poor_loop_stopped(&loop));
	if (!ret) {
		app_stop(&app, &loop);
		while (!ret && !app.down) {
			app_idle(&app);
			ret = poor_loop_run_once(&loop);
		}
	}
	if (!ret)
		ret = poor_loop_drain(&loop, poor_loop_now() + DRAIN_TIMEOUT_NS);
	if (ret) {
		printerrln("loop: ", strerror(-ret));
		poor_loop_exit(&loop);
		return 1;
	}
	println("drained");
	app_destroy(&app);
	poor_loop_exit(&loop);
	return app.status;
}
