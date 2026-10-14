/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#include "harness.h"

constexpr unsigned TURNS = 16;

/* Relaxed atomics give TSan no happens-before, like threads that only talk through rings */
static atomic_uint turn;

static void wait_turn(unsigned i)
{
	while (atomic_load_explicit(&turn, memory_order_relaxed) != i)
		sched_yield();
}

static void pass_turn(unsigned i)
{
	atomic_store_explicit(&turn, i, memory_order_relaxed);
}

static void take_turns(unsigned first)
{
	for (unsigned i = first; i < TURNS; i += 2) {
		struct poor_loop loop;
		struct rec rec;

		wait_turn(i);
		loop_init(&loop, 8);
		rec_init(&rec, rec_complete);
		arm_nop(&loop, &rec.op);
		drain(&loop, &rec.op);
		CHECK_EQ(rec.calls, 1);
		poor_loop_exit(&loop);
		pass_turn(i + 1);
	}
}

static void *odd_turns(void *)
{
	take_turns(1);
	return nullptr;
}

/* A new ring often gets the memory of the one closed before, here by another thread */
static int test_thread_loops_in_turn(void)
{
	pthread_t thread;

	pass_turn(0);
	CHECK_EQ(pthread_create(&thread, nullptr, odd_turns, nullptr), 0);
	take_turns(0);
	CHECK_EQ(pthread_join(thread, nullptr), 0);
	return 0;
}

static const struct test tests[] = {
	TEST(thread_loops_in_turn),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
