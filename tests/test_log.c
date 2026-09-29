/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include <pthread.h>

#include "harness.h"

static struct poor_loop *logged_loop;
static char logged[128];
static int logs, other_logs;

static void log_record(struct poor_loop *loop, const char *fmt, va_list ap)
{
	logged_loop = loop;
	vsnprintf(logged, sizeof(logged), fmt, ap);
	logs++;
}

static void log_other(struct poor_loop *, const char *, va_list)
{
	other_logs++;
}

static void sq_short(unsigned entries)
{
	struct poor_loop loop;
	struct rec recs[2];

	loop_init(&loop, entries);
	rec_init(&recs[0], rec_complete);
	rec_init(&recs[1], rec_complete);
	arm_nop(&loop, &recs[0].op);
	CHECK_EQ(poor_loop_check_sq_space_or_submit(&loop, entries), 0);
	arm_nop(&loop, &recs[1].op);
	drain(&loop, &recs[0].op, &recs[1].op);
	poor_loop_exit(&loop);
}

static int test_log_sq_space(void)
{
	struct poor_loop loop;
	struct rec recs[8];
	size_t i;

	logs = 0;
	poor_loop_log_function_set(log_record);
	loop_init(&loop, 4);
	for (i = 0; i < ARRAY_SIZE(recs); i++)
		rec_init(&recs[i], rec_complete);
	for (i = 0; i < 3; i++)
		arm_nop(&loop, &recs[i].op);
	CHECK_EQ(poor_loop_check_sq_space_or_submit(&loop, 1), 0);
	CHECK_EQ(logs, 0);
	CHECK_EQ(poor_loop_check_sq_space_or_submit(&loop, 2), 0);
	CHECK_EQ(logs, 1);
	CHECK(logged_loop == &loop);
	CHECK(!strcmp(logged, "SQ has 1 of 4 entries free, 2 needed: submitting"));
	for (i = 0; i < 3; i++)
		drain(&loop, &recs[i].op);
	for (i = 3; i < 7; i++)
		arm_nop(&loop, &recs[i].op);
	CHECK_EQ(logs, 1);
	arm_nop(&loop, &recs[7].op);
	CHECK_EQ(logs, 2);
	CHECK(!strcmp(logged, "SQ has 0 of 4 entries free, 1 needed: submitting"));
	for (i = 3; i < ARRAY_SIZE(recs); i++)
		drain(&loop, &recs[i].op);
	poor_loop_log_function_set(nullptr);
	poor_loop_exit(&loop);
	sq_short(2);
	CHECK_EQ(logs, 2);
	return 0;
}

static void *other_thread(void *)
{
	poor_loop_log_function_set(log_other);
	sq_short(2);
	return nullptr;
}

static int test_log_thread(void)
{
	pthread_t thread;

	logs = 0;
	other_logs = 0;
	poor_loop_log_function_set(log_record);
	CHECK_EQ(pthread_create(&thread, nullptr, other_thread, nullptr), 0);
	CHECK_EQ(pthread_join(thread, nullptr), 0);
	CHECK_EQ(other_logs, 1);
	CHECK_EQ(logs, 0);
	sq_short(2);
	CHECK_EQ(logs, 1);
	CHECK_EQ(other_logs, 1);
	poor_loop_log_function_set(nullptr);
	return 0;
}

const struct test tests[] = {
	TEST(log_sq_space),
	TEST(log_thread),
};

const size_t tests_count = ARRAY_SIZE(tests);
