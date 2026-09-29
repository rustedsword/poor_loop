/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "harness.h"

#define TIMERS 256
#define TIMER_STEPS 20'000

static int test_timer_init(void)
{
	struct poor_loop_timer lit = POOR_LOOP_TIMER_INIT(tick_fire), timer;

	CHECK(lit.fire == tick_fire);
	CHECK(!poor_loop_timer_armed(&lit));
	CHECK_EQ(lit.deadline, 0);
	memset(&timer, 0xa5, sizeof(timer));
	poor_loop_timer_init(&timer, tick_fire);
	CHECK(timer.fire == tick_fire);
	CHECK(!poor_loop_timer_armed(&timer));
	CHECK_EQ(timer.deadline, 0);
	return 0;
}

static int test_timer(void)
{
	struct poor_loop loop;
	struct tick tick;
	uint64_t deadline;

	loop_init(&loop, 8);
	tick_init(&tick, stop_fire, 1);
	CHECK(!poor_loop_timer_armed(&tick.timer));
	deadline = poor_loop_now() + 2'000'000;
	poor_loop_timer_arm(&loop, &tick.timer, deadline);
	CHECK(poor_loop_timer_armed(&tick.timer));
	CHECK_EQ(tick.timer.deadline, deadline);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(tick.fired, 1);
	CHECK(!tick.armed);
	CHECK(tick.at >= deadline);
	CHECK(!poor_loop_timer_armed(&tick.timer));
	poor_loop_exit(&loop);
	return 0;
}

static int test_timer_order(void)
{
	static const uint64_t deadlines[] = { 30, 10, 20, 10, 40 };
	struct poor_loop_timer stopper = POOR_LOOP_TIMER_INIT(halt);
	struct tick ticks[ARRAY_SIZE(deadlines)];
	struct poor_loop loop;

	loop_init(&loop, 8);
	foreach_array_index(ticks, i) {
		tick_init(&ticks[i], tick_fire, i + 1);
		poor_loop_timer_arm(&loop, &ticks[i].timer, deadlines[i]);
	}
	poor_loop_timer_arm(&loop, &ticks[4].timer, 5);
	poor_loop_timer_arm(&loop, &ticks[1].timer, 10);
	poor_loop_timer_arm(&loop, &stopper, 100);
	tick_order = 0;
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(tick_order, 54231);
	poor_loop_exit(&loop);
	return 0;
}

struct pair {
	struct tick first;
	struct tick second;
};

static void disarm_second(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	struct pair *pair = container_of(timer, struct pair, first.timer);

	tick_fire(loop, timer);
	poor_loop_timer_disarm(&pair->second.timer);
}

static int test_timer_disarm(void)
{
	struct poor_loop_timer stopper = POOR_LOOP_TIMER_INIT(halt);
	struct poor_loop loop;
	struct pair pair;
	struct tick tick;

	loop_init(&loop, 8);
	tick_init(&tick, tick_fire, 1);
	poor_loop_timer_arm(&loop, &tick.timer, 1);
	poor_loop_timer_disarm(&tick.timer);
	CHECK(!poor_loop_timer_armed(&tick.timer));
	poor_loop_timer_disarm(&tick.timer);
	tick_init(&pair.first, disarm_second, 2);
	tick_init(&pair.second, tick_fire, 3);
	poor_loop_timer_arm(&loop, &pair.first.timer, 1);
	poor_loop_timer_arm(&loop, &pair.second.timer, 2);
	poor_loop_timer_arm(&loop, &stopper, 3);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(tick.fired, 0);
	CHECK_EQ(pair.first.fired, 1);
	CHECK_EQ(pair.second.fired, 0);
	CHECK(!poor_loop_timer_armed(&pair.second.timer));
	poor_loop_exit(&loop);
	return 0;
}

static void rearm_fire(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	tick_fire(loop, timer);
	poor_loop_timer_arm(loop, timer, 0);
}

static int test_timer_rearm(void)
{
	struct poor_loop loop;
	struct tick tick;
	struct rec rec;

	loop_init(&loop, 8);
	tick_init(&tick, rearm_fire, 1);
	rec_init(&rec, stop_complete);
	poor_loop_timer_arm(&loop, &tick.timer, 0);
	arm_nop(&loop, &rec.op);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(rec.calls, 1);
	CHECK(tick.fired >= 1);
	CHECK(poor_loop_timer_armed(&tick.timer));
	poor_loop_timer_disarm(&tick.timer);
	poor_loop_exit(&loop);
	return 0;
}

static void periodic_fire(struct poor_loop *loop, struct poor_loop_timer *timer)
{
	struct tick *tick = container_of(timer, struct tick, timer);

	tick_fire(loop, timer);
	CHECK(tick->at >= timer->deadline);
	if (tick->fired < 5)
		poor_loop_timer_arm(loop, timer, timer->deadline + 1'000'000);
	else
		poor_loop_stop(loop);
}

static int test_timer_periodic(void)
{
	struct poor_loop loop;
	struct tick tick;
	uint64_t start;

	loop_init(&loop, 8);
	tick_init(&tick, periodic_fire, 1);
	start = poor_loop_now();
	poor_loop_timer_arm(&loop, &tick.timer, start + 1'000'000);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(tick.fired, 5);
	CHECK(tick.at >= start + 5'000'000);
	poor_loop_exit(&loop);
	return 0;
}

static int test_timer_wait(void)
{
	struct poor_loop loop;
	struct rec reader;
	struct tick tick;
	char buf[8];
	int fds[2];

	make_pipe(&fds);
	loop_init(&loop, 8);
	rec_init(&reader, rec_complete);
	tick_init(&tick, stop_fire, 1);
	arm_read(&loop, &reader.op, fds[0], buf);
	poor_loop_timer_arm(&loop, &tick.timer, poor_loop_now() + 2'000'000);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(tick.fired, 1);
	CHECK_EQ(reader.calls, 0);
	CHECK(reader.op.pending);

	poor_loop_timer_arm(&loop, &tick.timer, poor_loop_now() + 10'000'000'000);
	CHECK_EQ(write(fds[1], "x", 1), 1);
	drain(&loop, &reader.op);
	CHECK_EQ(reader.res, 1);
	CHECK_EQ(tick.fired, 1);
	CHECK(poor_loop_timer_armed(&tick.timer));
	poor_loop_timer_disarm(&tick.timer);
	poor_loop_exit(&loop);
	close_pipe(&fds);
	return 0;
}

static int test_timer_stop(void)
{
	struct poor_loop loop;
	struct tick tick;

	loop_init(&loop, 8);
	tick_init(&tick, stop_fire, 1);
	poor_loop_timer_arm(&loop, &tick.timer, poor_loop_now() + 1'000'000);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(tick.fired, 1);
	poor_loop_exit(&loop);
	return 0;
}

struct stamped {
	struct poor_loop_timer timer;
	unsigned seq;
};

static uint64_t fired_deadline;
static unsigned fired_seq, fired_count;

static void stamped_fire(struct poor_loop *, struct poor_loop_timer *timer)
{
	struct stamped *stamped = container_of(timer, struct stamped, timer);

	CHECK(timer->deadline >= fired_deadline);
	if (timer->deadline == fired_deadline)
		CHECK(stamped->seq > fired_seq);
	fired_deadline = timer->deadline;
	fired_seq = stamped->seq;
	fired_count++;
}

static void check_sorted(struct poor_loop *loop, unsigned armed)
{
	uint64_t deadline = 0;
	unsigned count = 0, seq = 0;

	poor_list_foreach(&loop->timers, timer) {
		struct stamped *stamped = container_of(timer, struct stamped, timer);

		CHECK(timer->deadline >= deadline);
		if (timer->deadline == deadline)
			CHECK(stamped->seq > seq);
		deadline = timer->deadline;
		seq = stamped->seq;
		count++;
	}
	CHECK_EQ(count, armed);
}

static uint32_t xorshift(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *state = x;
}

static int test_timer_sorted(void)
{
	struct stamped(*timers)[TIMERS] = calloc_array(timers);
	struct poor_loop_timer stopper = POOR_LOOP_TIMER_INIT(halt);
	unsigned armed = 0, seq = 0, step;
	struct poor_loop loop;
	uint32_t rng = 1;

	CHECK(timers);
	loop_init(&loop, 8);
	foreach_array_ref(timers, stamped)
		poor_loop_timer_init(&stamped->timer, stamped_fire);
	for (step = 0; step < TIMER_STEPS; step++) {
		struct stamped *stamped = &arr(timers)[xorshift(&rng) % ARRAY_SIZE(timers)];

		armed -= poor_loop_timer_armed(&stamped->timer);
		if (xorshift(&rng) % 4) {
			stamped->seq = ++seq;
			poor_loop_timer_arm(&loop, &stamped->timer, 1 + xorshift(&rng) % 64);
			armed++;
		} else {
			poor_loop_timer_disarm(&stamped->timer);
		}
		check_sorted(&loop, armed);
	}
	fired_deadline = 0;
	fired_seq = 0;
	fired_count = 0;
	poor_loop_timer_arm(&loop, &stopper, 1'000);
	CHECK_EQ(poor_loop_run(&loop), 0);
	CHECK_EQ(fired_count, armed);
	CHECK(poor_list_empty(&loop.timers));
	poor_loop_exit(&loop);
	free(timers);
	return 0;
}

static const struct test tests[] = {
	TEST(timer_init),
	TEST(timer),
	TEST(timer_order),
	TEST(timer_disarm),
	TEST(timer_rearm),
	TEST(timer_periodic),
	TEST(timer_wait),
	TEST(timer_stop),
	TEST(timer_sorted),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
