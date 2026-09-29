/* SPDX-License-Identifier: MIT */
#ifndef POOR_LOOP_TEST_HARNESS_H
#define POOR_LOOP_TEST_HARNESS_H

#include <errno.h>
#include <fcntl.h>
#include <poor_array.h>
#include <poor_loop.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SKIP 77

#define CHECK(cond) \
	do { \
		if (!(cond)) \
			fail(__FILE__, __LINE__, #cond); \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		long long a_ = (long long)(a), b_ = (long long)(b); \
		if (a_ != b_) \
			fail_eq(__FILE__, __LINE__, #a, #b, a_, b_); \
	} while (0)

struct mode {
	const char *name;
	unsigned flags;
};

struct rec {
	struct poor_loop_op op;
	int calls;
	int more;
	int res;
	unsigned flags;
};

#define drain(loop, ...) \
	drain_ops(loop, ARRAY_SIZE(((struct poor_loop_op *[]){ __VA_ARGS__ })), \
		  &(struct poor_loop_op *[]){ __VA_ARGS__ })

#define arm_read(loop, op, fd, buf) _arm_read(loop, op, fd, ARRAY_SIZE(buf), &auto_arr(buf))

struct tick {
	struct poor_loop_timer timer;
	int id;
	int fired;
	bool armed;
	uint64_t at;
};

struct test {
	const char *name;
	int (*fn)(void);
};

#define TEST(name) { #name, test_##name }

#define run_tests(args, tests) _run_tests(ARRAY_SIZE(args), &auto_arr(args), ARRAY_SIZE(tests), &auto_arr(tests))

extern const struct mode *mode;
extern uint64_t tick_order;

[[noreturn]] void fail(const char *file, int line, const char *expr);
[[noreturn]] void fail_eq(const char *file, int line, const char *a, const char *b, long long va, long long vb);
void setup_params(struct io_uring_params *p);
void loop_init(struct poor_loop *loop, unsigned entries);
void enable_ring(struct poor_loop *loop);
void make_pipe(int (*fds)[2]);
void close_pipe(int (*fds)[2]);
struct io_uring_sqe *get_sqe(struct poor_loop *loop, struct poor_loop_op *op);
void arm_nop(struct poor_loop *loop, struct poor_loop_op *op);
void _arm_read(struct poor_loop *loop, struct poor_loop_op *op, int fd, size_t len, char (*buf)[len]);
void drain_ops(struct poor_loop *loop, size_t count, struct poor_loop_op *(*ops)[count]);
void rec_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe);
void rec_init(struct rec *rec, poor_loop_complete_fn *complete);
void stop_complete(struct poor_loop *loop, struct poor_loop_op *op, const struct io_uring_cqe *cqe);
void tick_fire(struct poor_loop *loop, struct poor_loop_timer *timer);
void tick_init(struct tick *tick, poor_loop_timer_fn *fire, int id);
void stop_fire(struct poor_loop *loop, struct poor_loop_timer *timer);
void halt(struct poor_loop *loop, struct poor_loop_timer *timer);
int _run_tests(size_t argc, char *(*argv)[argc], size_t count, const struct test (*tests)[count]);

#endif
