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
	vsnprintf(logged, ARRAY_SIZE_BYTES(logged), fmt, ap);
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
	foreach_array_ref(recs, rec)
		rec_init(rec, rec_complete);
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

	logs = 0;
	poor_loop_log_function_set(log_record);
	loop_init(&loop, 4);
	foreach_array_ref(recs, rec)
		rec_init(rec, rec_complete);
	foreach_array_ref(arrview_first(3, recs), rec)
		arm_nop(&loop, &rec->op);
	CHECK_EQ(poor_loop_check_sq_space_or_submit(&loop, 1), 0);
	CHECK_EQ(logs, 0);
	CHECK_EQ(poor_loop_check_sq_space_or_submit(&loop, 2), 0);
	CHECK_EQ(logs, 1);
	CHECK(logged_loop == &loop);
	CHECK(!strcmp(logged, "SQ has 1 of 4 entries free, 2 needed: submitting"));
	foreach_array_ref(arrview_first(3, recs), rec)
		drain(&loop, &rec->op);
	foreach_array_ref(arrview(3, 4, recs), rec)
		arm_nop(&loop, &rec->op);
	CHECK_EQ(logs, 1);
	arm_nop(&loop, &recs[7].op);
	CHECK_EQ(logs, 2);
	CHECK(!strcmp(logged, "SQ has 0 of 4 entries free, 1 needed: submitting"));
	foreach_array_ref(arrview_cfront(3, recs), rec)
		drain(&loop, &rec->op);
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

static const struct test tests[] = {
	TEST(log_sq_space),
	TEST(log_thread),
};

int main(int argc, char **argv)
{
	return run_tests(array_ptr(argv, argc), tests);
}
