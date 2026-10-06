/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <poor_loop.h>
#include <poor_loop_group.h>
#include <poor_stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS 1'000'000

/*
 * Two allocated timers in one group: 'tick' fires every 100 ms, 'timeout'
 * gives up after timeout_ms. The one that finishes first stops both, and the
 * loop calls 'done'.
 *
 * 'stop' is a plain timer outside the group that stops both timers after
 * stop_ms. Its last disarm drops the counter to zero and returns true, so it
 * calls 'done' itself.
 *
 * Usage: group_timers [timeout_ms] [stop_ms]
 */
poor_loop_group_declare(ticker_io);

struct timer_req {
	struct poor_loop_timer timer;
	ticker_io *io;
};

struct ticker {
	ticker_io io;
	struct timer_req *tick, *timeout;
	struct poor_loop_timer stop;
	unsigned ticks;
};

poor_loop_group_attach(ticker_io, struct timer_req, io, timer, tick_fire);
poor_loop_group_attach(ticker_io, struct timer_req, io, timer, timeout_fire);

static struct ticker *ticker_of(ticker_io *io)
{
	return container_of(io, struct ticker, io);
}

static struct timer_req *new_timer(ticker_io *io, poor_loop_timer_fn *fire)
{
	struct timer_req *req = malloc(sizeof(*req));

	if (req) {
		poor_loop_timer_init(&req->timer, fire);
		req->io = io;
	}
	return req;
}

/* Disarm and free. Also called from the timer's own callback, where it is not armed anymore. */
[[nodiscard]] static bool free_timer(struct timer_req **req)
{
	bool idle = poor_loop_group_timer_disarm((*req)->io, &(*req)->timer);

	free(*req);
	*req = nullptr;
	return idle;
}

static void stop_timers(struct poor_loop *loop, struct ticker *t)
{
	bool idle = free_timer(&t->tick);

	idle |= free_timer(&t->timeout);
	if (idle)
		poor_loop_group_call_done(ticker_io, loop, &t->io);
}

POOR_LOOP_GROUP_TIMER(tick_fire, loop, io, timer)
{
	struct ticker *t = ticker_of(io);

	println("tick ", ++t->ticks);
	if (t->ticks < 5)
		poor_loop_group_timer_arm(loop, io, timer, poor_loop_now() + 100 * MS);
	else
		stop_timers(loop, t);
}

POOR_LOOP_GROUP_TIMER(timeout_fire, loop, io, timer)
{
	println("timeout");
	stop_timers(loop, ticker_of(io));
}

static void stop_fire(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	println("stop");
	stop_timers(loop, container_of(timer, struct ticker, stop));
}

POOR_LOOP_GROUP_DONE(ticker_io, loop, io)
{
	struct ticker *t = ticker_of(io);

	println("done after ", t->ticks, " ticks");
	poor_loop_timer_disarm(&t->stop);
	poor_loop_stop(loop);
}

int main(int argc, char **argv)
{
	struct ticker t = {
		.io = POOR_LOOP_GROUP_INIT,
		.stop = POOR_LOOP_TIMER_INIT(stop_fire),
	};
	uint64_t timeout = argc > 1 ? strtoull(argv[1], nullptr, 10) : 1000;
	uint64_t stop = argc > 2 ? strtoull(argv[2], nullptr, 10) : 2000;
	struct io_uring_params params = {};
	struct poor_loop loop;
	uint64_t now;
	int ret;

	ret = poor_loop_init(&loop, 8, &params);
	if (ret) {
		printerrln("poor_loop_init: ", strerror(-ret));
		return 1;
	}
	t.tick = new_timer(&t.io, tick_fire);
	t.timeout = new_timer(&t.io, timeout_fire);
	if (!t.tick || !t.timeout) {
		perror("malloc");
		free(t.tick);
		free(t.timeout);
		poor_loop_exit(&loop);
		return 1;
	}
	now = poor_loop_now();
	poor_loop_group_timer_arm(&loop, &t.io, &t.tick->timer, now + 100 * MS);
	poor_loop_group_timer_arm(&loop, &t.io, &t.timeout->timer, now + timeout * MS);
	poor_loop_timer_arm(&loop, &t.stop, now + stop * MS);
	ret = poor_loop_run(&loop);
	poor_loop_exit(&loop);
	return ret != 0;
}
